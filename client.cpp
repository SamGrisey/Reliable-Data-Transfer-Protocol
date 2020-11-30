#include <unistd.h>
#include <signal.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <poll.h>
#include <math.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <map>
#include "shared.h"
using namespace std; 

//global state vars
struct itimerval rdt_timer;
byte rdt_algorithm = SLOW_START;
double rdt_cwnd = RDT_INIT_CWND;
int rdt_ssthresh = RDT_INIT_SSTHRESH;
u16 rdt_connection_state = CONNECTION_IDLE, rdt_seqNum = 0, rdt_expectedNum = 0, rdt_cwnd_base_seqNum = 0, FIN_seqnum = 0;
byte rcv_buf[RDT_MAILBOX_SIZE];
struct sockaddr_in their_addr; /* connector addr */
int sockfd, filefd;
char *filename;
struct pollfd sock_pollfd;
map<u16,rdtHeader*> send_dict;
map<u16,u16> timesACKed_dict;
//srv2cli_dict: translates ackNums received from the server to the DATA packet seqNum they correspond to
map<u16,u16> srv2cli_dict;

int remainingBytesInCWND()
{
	//no wraparound
	if(((int)rdt_cwnd_base_seqNum + (int)rdt_cwnd) % RDT_MAX_SEQUENCE_NUM  > rdt_cwnd_base_seqNum)
		return (int)rdt_cwnd_base_seqNum + (int)rdt_cwnd - (int)rdt_seqNum;
	//wraparound with rdt_seqNum in "bottom part" of CWND (before RDT_MAX_SEQUENCE_NUM)
	else if((int)rdt_seqNum >= (int)rdt_cwnd_base_seqNum)
		return (RDT_MAX_SEQUENCE_NUM - (int)rdt_seqNum) + ((int)rdt_cwnd_base_seqNum + (int)rdt_cwnd) % RDT_MAX_SEQUENCE_NUM);
	//wraparound with rdt_seqNum in "upper part" of CWND (wrapped around RDT_MAX_SEQUENCE_NUM)
	else
		return (((int)rdt_cwnd_base_seqNum + (int)rdt_cwnd) % RDT_MAX_SEQUENCE_NUM) - (int)rdt_seqNum;
}

void quitHandler(int arg)
{
	close(sockfd); 
	exit(0);
}

void dipWithError(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

//sends the given packet and logs to console, does no bookkeeping
void sendPacket(rdtHeader* packet)
{
	write(sockfd, (void*)packet, 12 + packet->payloadSize);
	printf("SEND %hu %hu %d %d%s%s%s%s\n", packet->seqNum, packet->ackNum, (int)rdt_cwnd, rdt_ssthresh,
		((packet->flags & ACK_FLAG) ? " ACK" : ""),
		((packet->flags & SYN_FLAG) ? " SYN" : ""),
		((packet->flags & FIN_FLAG) ? " FIN" : ""),
		((packet->flags & DUP_FLAG) ? " DUP" : ""));
}

//Should send as much data as possible dictated by cwnd
int sendData()
{
	//send as long as at least 1 MSS is available
	while(remainingBytesInCWND() >= RDT_MSS){
		rdtHeader* packet = (rdtHeader*)malloc(524);
		int bytesRead = read(filefd, (void*)((u64)packet + 12), 512);
		
		//clear old packet from cache (should never actually happen)
		if(send_dict.count(rdt_seqNum))
			free(send_dict[rdt_seqNum]);
		
		//generate header and send packet
		packet->seqNum = rdt_seqNum;
		packet->ackNum = rdt_expectedNum;
		packet->payloadSize = bytesRead;
		packet->flags = ACK_FLAG;
		memset((void*)&packet->zero, 0, 5);
		sendPacket(packet);
		
		//bookkeeping and caching
		rdt_seqNum = (rdt_seqNum + bytesRead) % RDT_MAX_SEQUENCE_NUM;
		srv2cli_dict[rdt_seqNum] = packet->seqNum;
		timesACKed_dict[packet->seqNum] = 0;
		send_dict[packet->seqNum] = packet;
		
		//send FIN if all data sent
		if(bytesRead < 512){
			packet = (rdtHeader*)malloc(12);
			FIN_seqnum = rdt_seqNum;
			
			//clear old packet from cache (should never actually happen)
			if(send_dict.count(rdt_seqNum))
				free(send_dict[rdt_seqNum]);
			
			//generate header and send packet
			packet->seqNum = rdt_seqNum;
			packet->ackNum = rdt_expectedNum;
			packet->payloadSize = 0;
			packet->flags = FIN_FLAG;
			memset((void*)&packet->zero, 0, 5);
			sendPacket(packet);
			
			//bookkeeping and caching
			rdt_seqNum = (rdt_seqNum + 1) % RDT_MAX_SEQUENCE_NUM;
			srv2cli_dict[rdt_seqNum] = packet->seqNum;
			timesACKed_dict[packet->seqNum] = 0;
			send_dict[packet->seqNum] = packet;
			
			//enter AWAITING_FINACK
			rdt_connection_state = AWAITING_FINACK;
		}
	}
}

//retrans lost segment and enter fast recovery
int fastRetransmit()
{
	rdt_ssthresh = (((int)rdt_cwnd/2) > 1024 ? ((int)rdt_cwnd/2) : 1024);
	rdt_cwnd = (int)rdt_ssthresh + 1536;
	rdt_algorithm = FAST_RECOVERY;

	//Grab lost SeqNum from the dict and resend it
	rdtHeader* retransPacket = send_dict[rdt_cwnd_base_seqNum];
	retransPacket->flags = (retransPacket->flags)|DUP_FLAG;
	sendPacket(retransPacket);
	
	//restart RTO timer
	ualarm(500000, 0);
}

//basically a glorified call to exit() after a 2 second poll timeout because I dont wanna juggle return values back to main()
void finalize()
{
	//stop RTO timer
	ualarm(0, 0);
	
	//send final FINACK
	rdtHeader finack;
	rdtHeader msg;
	finack.seqNum = rdt_seqNum;
	finack.ackNum = rdt_expectedNum;
	finack.flags = ACK_FLAG;
	finack.payloadSize = 0;
	memset((void*)&finack.zero, 0, 5);
	sendPacket(&finack);
	
	//wait to exit
	while(1)
	{
		//wait 2 sec before exiting
		if(poll(&sock_pollfd, 1, 2000))
		{
			//received message
			//blindly resend the FINACK
			finack.flags = ACK_FLAG|DUP_FLAG;
			sendPacket(&finack);
		}
		else //idle for 2 sec; OK to exit(0)
		{
			exit(0);
		}
	}
}

//Grab buffered messages and process them
int receiveData()
{
	int bytesRead = read(sockfd, rcv_buf, RDT_MAILBOX_SIZE);
	
	//iterate through received headers
	for(int i = 0; i < bytesRead; i += 12){
		int packetsCumulativelyAcked = 0;
		//dereference header
		rdtHeader *header = (rdtHeader*)((u64)rcv_buf + i);
		
		//print the RECV message to stdout
		printf("RECV %hu %hu %d %d%s%s%s\n", header->seqNum, header->ackNum, (int)rdt_cwnd, rdt_ssthresh,
			((header->flags & ACK_FLAG) ? " ACK" : ""),
			((header->flags & SYN_FLAG) ? " SYN" : ""),
			((header->flags & FIN_FLAG) ? " FIN" : ""));
		
		//if AWAITING_FINACK: check if packet is the server-sent FINACK_FIN packet. If so, finalize the teardown
		if(rdt_connection_state == AWAITING_FINACK && header->flags == FIN_FLAG && header->ackNum == (FIN_seqnum + 1) % RDT_MAX_SEQUENCE_NUM)
			finalize();
		
		//check to see if the ACK is for a packet currently thought to be out for delivery, if not, continue to the next header
		if(!srv2cli_dict.count(header->ackNum))
			continue;
		//at this point, this ACK packet is known to hold pertinent information about a currently un-ACKed DATA packet
		
		//increment timesACKed_dict entry and rdt_expectedNum
		timesACKed_dict[srv2cli_dict[header->ackNum]] = timesACKed_dict[srv2cli_dict[header->ackNum]] + 1;
		rdt_expectedNum = (header->seqNum + 1) % RDT_MAX_SEQUENCE_NUM;
		
		//clear the caches of all info to do with the DATA packets that this ACK packet cumulatively ACKs (everything before itself) and keep track of how many DATA packets we cleared
		while(1){
			//break if that was the last DATA packet cumulatively ACKed
			if(rdt_cwnd_base_seqNum == srv2cli_dict[header->ackNum]) break;
			
			//stop the RTO alarm
			ualarm(0, 0);
			
			//pull next DATA packet from cache
			rdtHeader* currentPacket = send_dict[rdt_cwnd_base_seqNum];
			
			//slide rdt_cwnd_base_seqNum forward and increment packetsCumulativelyAcked. erase caches
			timesACKed_dict.erase(rdt_cwnd_base_seqNum);
			send_dict.erase(rdt_cwnd_base_seqNum);
			srv2cli_dict.erase(header->ackNum);
			rdt_cwnd_base_seqNum = (rdt_cwnd_base_seqNum + currentPacket->payloadSize) % RDT_MAX_SEQUENCE_NUM;
			packetsCumulativelyAcked++;
			free(currentPacket);
		}
		
		//Handle additional duplicate ACKs while in FAST_RECOVERY
		if(rdt_algorithm == FAST_RECOVERY && packetsCumulativelyAcked == 0){
			rdt_cwnd += RDT_MSS;
			sendData();
		}
		
		//Perform fast retransmit and enter FAST_RECOVERY if this is the 4th ACK (meaning 3rd duplicate ACK) for the base DATA packet
		if(timesACKed_dict[srv2cli_dict[header->ackNum]] == 4)
			fastRetransmit();
		
		//update rdt_cwnd, rdt_ssthresh, and rdt_algorithm. Send more data based on how many packets were ACKed
		for(int i = 0; i < packetsCumulativelyAcked; i++){
			switch(rdt_algorithm)
			{
				case SLOW_START:
					rdt_cwnd += RDT_MSS;
					//change to congestion avoidance if we need to
					if((int)rdt_cwnd == rdt_ssthresh)
						rdt_algorithm = CONGESTION_AVOIDANCE;
					sendData();
					break;
				case CONGESTION_AVOIDANCE:
					if((int)rdt_cwnd < RDT_MAX_CWND)
						rdt_cwnd = (rdt_cwnd + (RDT_MSS * RDT_MSS)/rdt_cwnd >= RDT_MAX_CWND) ? RDT_MAX_CWND : rdt_cwnd + (RDT_MSS * RDT_MSS)/rdt_cwnd;
					sendData();
					break;
				case FAST_RECOVERY:
					//upon receiving a non-duplicate ACK while in FAST_RECOVERY (duplicata case handled above)
					rdt_cwnd = (double)rdt_ssthresh;
					rdt_algorithm = CONGESTION_AVOIDANCE;
					sendData();
					break;
			}
		}
		
		//restart RTO alarm if we stopped it while processing non-duplicate ACKS
		if(packetsCumulativelyAcked > 0)
			ualarm(500000, 0);
	}
}

//Does RTO timeout logic
void timeoutHandler(int arg)
{
	//re-register signal handler
	signal(SIGALRM, timeoutHandler);
	
	//adjust rdt_cwnd and rdt_ssthresh
	rdt_cwnd = RDT_MSS;
	rdt_ssthresh = (((int)rdt_cwnd/2) > 1024 ? ((int)rdt_cwnd/2) : 1024);
	
	//retransmit timed out packet
	rdtHeader* retransPacket = send_dict[rdt_cwnd_base_seqNum];
	retransPacket->flags = (retransPacket->flags)|DUP_FLAG;
	sendPacket(retransPacket);
	
	//restart timer
	ualarm(500000, 0);
}

//setup RDT connection
void doHandshake()
{
	rdt_connection_state = CONNECTION_IDLE;
	rdtHeader syn;
	rdtHeader synack;
	rdt_seqNum = (u16)(rand() % RDT_MAX_SEQUENCE_NUM);
	rdt_cwnd_base_seqNum = rdt_seqNum;
	
	//build and send SYN header
	syn.seqNum = rdt_seqNum;
	syn.ackNum = 0;
	syn.payloadSize = 0;
	syn.flags = SYN_FLAG;
	memset((void*)&syn.zero, 0, 5);
	sendPacket(&syn);
	rdt_seqNum = (rdt_seqNum + 1) % RDT_MAX_SEQUENCE_NUM;
	
	//retransmit until valid reply
	while(1)
	{
		//wait 0.5 sec for SYNACK to arrive before retransmitting
		if(poll(&sock_pollfd, 1, 500))
		{
			read(sockfd, (void*)&synack, 12);
			//print the RECV message to stdout
			printf("RECV %hu %hu %d %d%s%s%s\n", synack->seqNum, synack->ackNum, (int)rdt_cwnd, rdt_ssthresh,
				((synack->flags & ACK_FLAG) ? " ACK" : ""),
				((synack->flags & SYN_FLAG) ? " SYN" : ""),
				((synack->flags & FIN_FLAG) ? " FIN" : ""));
				
			//If valid, update stare vars and break with CONNECTION_ACTIVE
			if(synack.flags == SYN_FLAG|ACK_FLAG && synack.ackNum == rdt_seqNum){
				rdt_expectedNum = (synack.seqNum + 1) % RDT_MAX_SEQUENCE_NUM;
				rdt_connection_state = CONNECTION_ACTIVE;
				break;
			}
		}
		else //idle for 0.5 sec, retransmit SYN
		{
			syn.flags = SYN_FLAG|DUP_FLAG;
			sendPacket(&syn);
		}
	}
}

int main(int argc, char** argv)
{
	int portnum;
	struct hostent *server_info; //for DNS lookup
	socklen_t sin_size;

	/* create a socket */
	if ((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) == -1)
		dipWithError("ERROR: Cannot create socket.");


	//register handlers
	signal(SIGTERM, quitHandler);
	signal(SIGQUIT, quitHandler);
	signal(SIGINT, quitHandler); 
	signal(SIGALRM, timeoutHandler);

	//set port
	if(argc != 4 || (portnum = atoi(argv[2])) < 1025 || portnum > 65535)
		dipWithError("ERROR: Invalid port specified.");

	//resolve hostname
	if((server_info = gethostbyname(argv[1])) == NULL)
		dipWithError("ERROR: Hostname could not be resolved.");
	cout << server_info << '\n'; 
	//open file
	if((filefd = open(argv[3], O_RDONLY)) == -1)
		dipWithError("ERROR: Cannot open specified input file.");

	//initialize sock_pollfd
	sock_pollfd.events = POLLIN;
	sock_pollfd.fd = sockfd;

	/* set the address info */
	their_addr.sin_family = AF_INET;
	their_addr.sin_port = htons(portnum); /* short, network byte order */
	//memcpy(server_info->h_addr, &their_addr.sin_addr.s_addr, server_info->h_length);
	their_addr.sin_addr.s_addr = INADDR_ANY;
	memset(their_addr.sin_zero, '\0', sizeof(their_addr.sin_zero));
	if ((connect(sockfd, (const struct sockaddr*) &their_addr, sizeof(their_addr))) == -1)
		dipWithError("ERROR: Cannot connect to server."); 		
	
	//begin handshake
	doHandshake(handflag); 
	
	//main data transfer loop
	sendData();
	ualarm(500000, 0);
	while(1)
	{
		//wait 10 sec for data to arrive
		if(poll(&sock_pollfd, 1, 10000))
		{
			receiveData();
		}
		else //idle for 10 sec
		{
			exit(1);
		}
	}
}