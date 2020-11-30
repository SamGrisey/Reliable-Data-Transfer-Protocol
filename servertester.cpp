#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
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

void dipWithError(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

int main(int argc, char** argv)
{
	int portnum;
	rdtHeader head;
	char buf[36];
	struct hostent *server_info; //for DNS lookup
	struct sockaddr_in my_addr; /* my address */
	
	/* create a recieve socket */
	if ((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) == -1)
		dipWithError("ERROR: Cannot create recieve socket.");
	
	if((server_info = gethostbyname("localhost")) == NULL)
		dipWithError("ERROR: Hostname could not be resolved.");

	//initialize sock_pollfd
	sock_pollfd.events = POLLIN;
	
	/* set the address info */
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(7000);/* short, network byte order */
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	memset(my_addr.sin_zero, '\0', sizeof(my_addr.sin_zero));
	if ((connect(sockfd, (const struct sockaddr*) &my_addr, sizeof(my_addr))) == -1)
		dipWithError("ERROR: Cannot connect to server.");
	//set up sock_pollfd
	sock_pollfd.fd = sockfd;
		
	//SYN
	head.seqNum = 0;
	head.ackNum = 0;
	head.payloadSize = 0;
	head.flags = 0|SYN_FLAG;
	memset((void*)&head.zero, 0, 5);
	write(sockfd, (void*) &head, 12); 
	
	//Data 3
	head.seqNum = 17;
	head.ackNum = 1;
	head.payloadSize = 8;
	head.flags = 0|ACK_FLAG;
	memset((void*)&head.zero, 0, 5);
	memcpy((void*)buf, (void*)&head, 12);
	snprintf(&buf[12], 9, "qrstuvwx");
	write(sockfd, (void*)buf, 20);
	
	//Data 2
	head.seqNum = 9;
	head.ackNum = 1;
	head.payloadSize = 8;
	head.flags = 0|ACK_FLAG;
	memset((void*)&head.zero, 0, 5);
	memcpy((void*)buf, (void*)&head, 12);
	snprintf(&buf[12], 9, "ijklmnop");
	write(sockfd, (void*)buf, 20);
	
	//Data 4
	head.seqNum = 25;
	head.ackNum = 1;
	head.payloadSize = 2;
	head.flags = 0|ACK_FLAG;
	memset((void*)&head.zero, 0, 5);
	memcpy((void*)buf, (void*)&head, 12);
	snprintf(&buf[12], 3, "yz");
	write(sockfd, (void*)buf, 14);
	
	//Data 1
	head.seqNum = 1;
	head.ackNum = 1;
	head.payloadSize = 8;
	head.flags = 0|ACK_FLAG;
	memset((void*)&head.zero, 0, 5);
	memcpy((void*)buf, (void*)&head, 12);
	snprintf(&buf[12], 9, "abcdefghijkl");
	write(sockfd, (void*)buf, 20);

	//FIN
	head.seqNum = 27;
	head.ackNum = 8;
	head.payloadSize = 0;
	head.flags = 0|FIN_FLAG|ACK_FLAG;
	memset((void*)&head.zero, 0, 5);
	write(sockfd, (void*) &head, 12);

	head.seqNum = 28;
	head.ackNum = 10;
	head.payloadSize = 0;
	head.flags = 0|ACK_FLAG;
	memset((void*)&head.zero, 0, 5);
	write(sockfd, (void*) &head, 12);
}

