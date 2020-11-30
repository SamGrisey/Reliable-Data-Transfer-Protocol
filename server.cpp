#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <map>
#include "shared.h"
using namespace std;

//global state vars
u16 rdt_connection_state = CONNECTION_IDLE, rdt_seqNum = 0, rdt_expectedNum = 0;
u16 FINACK_seqNum = 0;
byte rcv_buf[RDT_MAILBOX_SIZE];
struct sockaddr_in cli_addr; /* client address */
socklen_t cli_addr_len = sizeof(struct sockaddr_in);
	
int sockfd, filefd, currentFilenum = 0;
char filename[32];
struct pollfd sock_pollfd;
std::map<u16, char*> server_cache;

void quitHandler(int arg)
{
	if(rdt_connection_state != CONNECTION_IDLE){
		close(filefd);
		filefd = open(filename, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
		dprintf(filefd, "INTERRUPT");
		close(filefd);
	}
	exit(0);
}

void sendACK(u16 ackNum, u16 seqNum, byte flags)
{
	rdtHeader header;
	//build header
	header.seqNum = seqNum;
	header.ackNum = ackNum;
	header.payloadSize = 0;
	header.flags = 0|flags;
	memset((void*)header.zero, 0, 5);
	//write to socket and stdout
	sendto(sockfd, (void*)&header, 12, NULL, (struct sockaddr*)&cli_addr, cli_addr_len);
	printf("SEND %hu %hu 0 0%s%s%s%s\n", header.seqNum, header.ackNum,
		((header.flags & ACK_FLAG) ? " ACK" : ""),
		((header.flags & SYN_FLAG) ? " SYN" : ""),
		((header.flags & FIN_FLAG) ? " FIN" : ""),
		((header.flags & DUP_FLAG) ? " DUP" : ""));
	//Only increment rdt_seqNum if the sent message wasn't a resend
	rdt_seqNum = (seqNum == rdt_seqNum) ? ((rdt_seqNum + 1) % RDT_MAX_SEQUENCE_NUM) : rdt_seqNum;
}

void timeoutHandler(int arg)
{
	signal(SIGALRM, timeoutHandler);
	//resend FINACK
	if(rdt_connection_state == AWAITING_FINACK){
		sendACK(rdt_expectedNum, FINACK_seqNum, ACK_FLAG|DUP_FLAG);
		sendACK(rdt_expectedNum, FINACK_seqNum + 1, FIN_FLAG|DUP_FLAG);
	}
	//resend SYNACK
	else if(rdt_connection_state == CONNECTION_ACTIVE){
		sendACK(rdt_expectedNum, 0, SYN_FLAG|ACK_FLAG|DUP_FLAG);
	}
	//restart timer
	ualarm(500000, 0);
}

void dipWithError(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

//Processes all consecutive messages in the dictionary starting with rdt_expectedNum
//Returns 0 if there is more data on the way, Returns 1 when connection has been properly closed
int processBuffer()
{
	while(server_cache.count(rdt_expectedNum)){
		u16 key = rdt_expectedNum;
		rdtHeader* header = (rdtHeader*)server_cache[key];
		
		//NORMAL OPERATION
		if(rdt_connection_state == CONNECTION_ACTIVE){
			//Begin FIN sequence
			if(header->flags & FIN_FLAG){
				rdt_expectedNum = (rdt_expectedNum + 1) % RDT_MAX_SEQUENCE_NUM;
				FINACK_seqNum = rdt_seqNum;
				sendACK(rdt_expectedNum, rdt_seqNum, ACK_FLAG);
				sendACK(rdt_expectedNum, rdt_seqNum, FIN_FLAG);
				rdt_connection_state = AWAITING_FINACK;
				//start timer
				ualarm(500000, 0);
			}else{
				//Write data and send in-order ACK
				write(filefd, (char*)((unsigned long)header + 12), header->payloadSize);
				rdt_expectedNum = (rdt_expectedNum + header->payloadSize) % RDT_MAX_SEQUENCE_NUM;
				sendACK(rdt_expectedNum, rdt_seqNum, ACK_FLAG);
			}
		//CONNECTION CLOSING OPERATION
		} else if (rdt_connection_state == AWAITING_FINACK) {
			//close connection if this is the ACK for server-sent FINACK
			if((header->flags & ACK_FLAG) && (header->ackNum == FINACK_seqNum + 2) && !(header->flags & FIN_FLAG) && !(header->flags & SYN_FLAG)){
				//stop timer and return 1 (connection closed)
				ualarm(0, 0);
				rdt_connection_state = CONNECTION_IDLE;
				return 1;
			}
		}
		
		//clear the message from the cache
		free(header);
		server_cache.erase(key);
	}
	return 0;
}

//simplified TCP, caches and acks messages and calls for processing on consecutive chunks
//Returns 0 if there is more data on the way, Returns 1 when connection has been properly closed
int receiveData()
{
	int bytesRead = read(sockfd, rcv_buf, RDT_MAILBOX_SIZE);
	for(int i = 0; i < bytesRead;){
		//dereference header
		rdtHeader *header = (rdtHeader*)((u64)rcv_buf + i);
		u16 size = header->payloadSize;
		
		//encapsulate and cache message
		char* cacheBuffer = (char*)malloc(12 + size);
		memcpy((void*)cacheBuffer, (void*)((u64)rcv_buf + i), 12 + size);
		//clear old duplicate data
		if(server_cache.count(header->seqNum)){
			free(server_cache[header->seqNum]);
			server_cache.erase(header->seqNum);
		}
		server_cache[header->seqNum] = cacheBuffer;
		
		//print to stdout
		printf("RECV %hu %hu 0 0%s%s%s\n", header->seqNum, header->ackNum,
			((header->flags & ACK_FLAG) ? " ACK" : ""),
			((header->flags & SYN_FLAG) ? " SYN" : ""),
			((header->flags & FIN_FLAG) ? " FIN" : ""));
		
		//process the contiguous chunk of messages
		if(header->seqNum == rdt_expectedNum){
			//stop timer
			ualarm(0, 0);
			if(processBuffer())
				return 1;
		}else
			//send a dup ack if segment was out of order
			sendACK(rdt_expectedNum, rdt_seqNum, ACK_FLAG);
			
		//move cursor along to next header
		i += (size + 12);
	}
	return 0;
}

int main(int argc, char** argv)
{
	int portnum;
	struct sockaddr_in my_addr; /* my address */
	
	/* create a recieve socket */
	if ((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) == -1)
		dipWithError("ERROR: Cannot create recieve socket.");
	
	//register handlers
	signal(SIGTERM, quitHandler);
	signal(SIGQUIT, quitHandler);
	signal(SIGALRM, timeoutHandler);
	
	//read port
	if(argc != 2 || (portnum = atoi(argv[1])) < 1025 || portnum > 65535)
		dipWithError("ERROR: Invalid port specified.");
	
	//initialize sock_pollfd
	sock_pollfd.events = POLLIN;
	
	/* set the address info */
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(portnum); /* short, network byte order */
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	memset(my_addr.sin_zero, '\0', sizeof(my_addr.sin_zero));
	
	/* bind the socket */
	if (bind(sockfd, (struct sockaddr*)&my_addr, sizeof(struct sockaddr)) == -1)
		dipWithError("ERROR: Error binding to port.");
	
	//set up sock_pollfd
	sock_pollfd.fd = sockfd;
		
	//main loop
	while(1)
	{	
		//RESET STATE VARS
		FINACK_seqNum = 0;
		rdt_seqNum = 0;
		server_cache.clear();
		rdt_connection_state = CONNECTION_IDLE;
		memset((void*)&cli_addr, 0, sizeof(struct sockaddr));
		
		//ESTABLISH CONNECTION
		//block until a header is available (gross spin wait just in case this thing doesn't block. Apparently it's system-dependent)
		while(recvfrom(sockfd, rcv_buf, 12, NULL, (struct sockaddr*)&cli_addr, &cli_addr_len) < 0);
		//process valid SYN header else ignore the entire buffer
		if(((rdtHeader*)rcv_buf)->flags & SYN_FLAG && !(((rdtHeader*)rcv_buf)->flags & FIN_FLAG) && !(((rdtHeader*)rcv_buf)->flags & ACK_FLAG) && ((rdtHeader*)rcv_buf)->payloadSize == 0){
			rdt_connection_state = CONNECTION_ACTIVE;
			rdt_expectedNum = ((rdtHeader*)rcv_buf)-> seqNum + 1;
			//Print to stdout
			printf("RECV %hu %hu 0 0%s%s%s\n", ((rdtHeader*)rcv_buf)->seqNum, ((rdtHeader*)rcv_buf)->ackNum,
			((((rdtHeader*)rcv_buf)->flags & ACK_FLAG) ? " ACK" : ""),
			((((rdtHeader*)rcv_buf)->flags & SYN_FLAG) ? " SYN" : ""),
			((((rdtHeader*)rcv_buf)->flags & FIN_FLAG) ? " FIN" : ""));
			sendACK(rdt_expectedNum, 0, SYN_FLAG|ACK_FLAG);
			//start timer to resend SYNACK if need be
			ualarm(500000, 0);
		}else continue;
		
		//CREATE FILE
		currentFilenum++;
		snprintf(filename, 32, "%d.file", currentFilenum);
		if((filefd = open(filename, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU)) < 0)
			dipWithError("ERROR: Cannot create output file.");
		
		//RECEIVE LOOP
		while(1)
		{
			//wait 10 sec for data to arrive
			if(poll(&sock_pollfd, 1, 10000))
			{
				//break after successfully handling FIN and closing connection
				if(receiveData() == 1)
					break;
			}
			else //idle for 10 sec
			{
				break;
			}
		}
		close(filefd);
	}
}
