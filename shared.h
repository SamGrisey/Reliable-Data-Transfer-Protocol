#define RDT_HEADER_SIZE 12
#define RDT_MAX_SEQUENCE_NUM 25600
#define RDT_MSS 524
#define RDT_INIT_CWND 512
#define RDT_MAX_CWND 10240
#define RDT_INIT_SSTHRESH 5120
#define RDT_MAILBOX_SIZE 5365760
#define ACK_FLAG 1
#define SYN_FLAG 2
#define FIN_FLAG 4
#define DUP_FLAG 8
#define CONNECTION_IDLE 0
#define CONNECTION_ACTIVE 1
#define AWAITING_FINACK 2
#define SLOW_START 0
#define CONGESTION_AVOIDANCE 1
#define FAST RECOVERY 2
#define RTO_TIMEOUT_USEC 500000

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char byte;

typedef struct rdt_Header 
{
	u16 seqNum, ackNum, payloadSize;
	byte flags;
	byte zero[5];
}rdtHeader;
