#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/sockios.h>
#endif
#include "common.h"
#include "crc.h"
#include "msgdispatch.h"
#include "remotesocket.h"
#include <cJSON.h>
#include <fcntl.h>
#include <malloc.h>

#if 0
#define DEBUGP printf
#else
#define DEBUGP(format, args...)
#endif

STRREMOTE_IPMAP remote_ipmap[] = {
	{SOCK_BACKEND_ID, "127.0.0.1", 7433, 7434}, {SOCK_REMOTEXMPP_ID, "127.0.0.1", 7640, 7641},
};

char app_name[128];

U32 msgcounter = 0;

typedef struct tag_SockRecMsgState {
	U8*			  recv_buff;
	U32			  headerpointer;
	U8			  headerring[SOCKET_HEADER_LEN];
	SOCKET_HEADER sock_header;
	U32			  datalen;
	U32			  msgid;
	U32			  headercount;
	U32			  state;
	U32			  recvcount;
} RMTSOCK_RECMSG_STATE;
typedef struct tag_SockNode {
	int					 fd;
	struct sockaddr_in   addr_in;
	pthread_mutex_t		 lock;
	char				 from[MSG_ID_SIZE];
	char				 to[MSG_ID_SIZE];
	RMTSOCK_RECMSG_STATE recstate;
	int					 socketype;
	int					 connectstate;
} RMTSOCK_CLIENT_NODE;

typedef struct tag_RemoteSockNode {
	int					 fd;
	struct sockaddr_in   addr_in;
	pthread_mutex_t		 lock;
	char				 from[MSG_ID_SIZE];
	char				 to[MSG_ID_SIZE];
	RMTSOCK_RECMSG_STATE recstate;
	int					 socketype;
	int					 connectstate;
} REMOTE_SOCK_CLIENT_NODE;

typedef struct tag_RemoteSockServerParam {
	int						  remote_port;
	int						  sockfd;
	struct sockaddr_in		  addr_in;
	REMOTESOCKET_CALLBACK_FUN callback;
	void*					  arg;
	pthread_t				  thread;
	pthread_mutex_t			  lock;
	REMOTE_SOCK_CLIENT_NODE   client[REMOTESOCKET_MAX_CLIENTS];
	fd_set					  readfds;
} REMOTE_SOCK_SERVER_PARAM;

typedef struct tag_RemoteSockClientParam {
	int						  sockfd;
	struct sockaddr_in		  addr;
	REMOTESOCKET_CALLBACK_FUN callback;
	void*					  arg;
	pthread_t				  thread;
	pthread_mutex_t			  lock;
	REMOTE_SOCK_CLIENT_NODE   client[REMOTESOCKET_MAX_SERVERS];
	fd_set					  readfds;
} REMOTE_SOCK_CLIENT_PARAM;

typedef struct tag_SockUDPParam {
	int						  sockfd;
	struct sockaddr_in		  addr_in;
	REMOTESOCKET_CALLBACK_FUN callback;
	void*					  arg;
	pthread_t				  thread;
	pthread_mutex_t			  lock;
	REMOTE_SOCK_CLIENT_NODE   client;
	fd_set					  readfds;
} SOCK_UDP_PARAM;

#define REMOTESOCKET_MAX_DST_PID 30
typedef struct tag_SockesParam {
	pthread_t				 remotesocket_receive_thread;
	pthread_t				 remotesocket_timer_thread;
	BOOL					 enableremotesrv;
	BOOL					 enabletcpsrv;
	BOOL					 enableudpsrv;
	PID_CONNECT				 conpid[REMOTESOCKET_MAX_DST_PID];
	U32						 conpidnum;
	char					 pid[MSG_ID_SIZE];
	REMOTE_SOCK_SERVER_PARAM remote_server;
	SOCK_UDP_PARAM			 udp_server;
	REMOTE_SOCK_CLIENT_PARAM remote_clients;
} SOCKS_PARAM;

static SOCKS_PARAM remotesocket_regs = {0};

#define REMOTESOCKET_SEND_QUEUE_SIZE 30
typedef struct {
	BOOL finish;
	char pid[MSG_ID_SIZE];
	U32  socket;
	U32  length;
	U32  waitack;
	U32  msgid;
	int  sockfd;
	U32  sendtype;
	U32  state;
} REMOTESOCKETSendQueueNode;
typedef struct tag_LocalSocketSendRegs {
	U32						  inp;
	U32						  outp;
	U32						  count;
	U32						  counter;
	REMOTESOCKETSendQueueNode queue[REMOTESOCKET_SEND_QUEUE_SIZE];
	pthread_mutex_t			  lock;
} REMOTESOCKETSendRegsType;
REMOTESOCKETSendRegsType remotesocket_sendRegs;

const int  rmtsocktypeorder[] = {REMOTESOCKET_SOCKTYPE_TCPSERVER, REMOTESOCKET_SOCKTYPE_SHORTTCP, REMOTESOCKET_SOCKTYPE_UDP, REMOTESOCKET_SOCKTYPE_LONGTCP};
static int debug_count		  = 0;
static int write_fd			  = -1;
U8 remotesocket_findipbypid(char* pid, char* ip, U16* tcp_port, U16* udp_port);
int remotesocket_findsocktypebypid(char* pid);

static void* remotesocket_listen(void*);
static void* remotesocket_time(void*);

static BOOL checkheader(SOCKET_HEADER* sock_header, U8* header, U32 pointer)
{
	int err_code				= 0;
	U32 i						= 0;
	U32 p						= 0;
	U8  buff[SOCKET_HEADER_LEN] = {0};
	U8  crc[2]					= {0};
	// BOOL res					 = TRUE;

	p = pointer;
	for (i = 0; i < SOCKET_HEADER_LEN; i++) {
		if (p >= SOCKET_HEADER_LEN) {
			p = 0;
		}
		buff[i] = header[p];
		p++;
	}

	SOCKET_HEADER* h = (SOCKET_HEADER*) buff;
	if (h->syncHeader.frameHeader.word != SOCKET_SYNC_HEADER)
		goto ERROR_HEADER;
	err_code = 1;
	crc_calc((U8*) (&h->msgHeader), MSG_HEADER_LENTH, crc);
	DEBUGP("receive message crc= %02x,%02x, cal=%02x,%02x\n", crc[0], crc[1], h->syncHeader.headCRC.bytes[2], h->syncHeader.headCRC.bytes[3]);
	if ((h->syncHeader.headCRC.bytes[2] != crc[0]) || (h->syncHeader.headCRC.bytes[3] != crc[1])) {
		goto ERROR_HEADER;
	}
	err_code = 2;
	if (0 == h->msgHeader.srcID[0] && 0 == h->msgHeader.srcID[1] && 0 == h->msgHeader.srcID[2] && 0 == h->msgHeader.srcID[3] && 0 == h->msgHeader.srcID[4] &&
		0 == h->msgHeader.srcID[5]) {
		goto ERROR_HEADER;
	}
	err_code = 3;
	if (0 == h->msgHeader.dstID[0] && 0 == h->msgHeader.dstID[1] && 0 == h->msgHeader.dstID[2] && 0 == h->msgHeader.dstID[3] && 0 == h->msgHeader.dstID[4] &&
		0 == h->msgHeader.dstID[5]) {
		goto ERROR_HEADER;
	}
	/*
	err_code = 4;
	if(0 == h->msgHeader.funcCode)
	{
			goto ERROR_HEADER;

	}
	err_code = 5;
	if(0 == h->msgHeader.operCode)
	{
			goto ERROR_HEADER;

	}
	*/
	err_code	 = 6;
	U32 iDataLen = MAKEFOURCC_BE4(h->msgHeader.dataLen[0], h->msgHeader.dataLen[1], h->msgHeader.dataLen[2], h->msgHeader.dataLen[3]);
	if (iDataLen < MSG_HEADER_LENTH) {
		goto ERROR_HEADER;
	}
	err_code = 7;
	memcpy((U8*) sock_header, buff, SOCKET_HEADER_LEN);
	return TRUE;

ERROR_HEADER:
	printf("=== Error head check. error code:%d, %d ====\n", err_code, h->syncHeader.frameHeader.word);
	return FALSE;
}
BOOL remotesocket_tcp_close(int sockfd)
{
	if (sockfd < 0)
		return FALSE;

	int  i			= 0;
	int  leftunsend = 0;
	BOOL res		= TRUE;

	for (i = 0; i < 50; i++) {
		leftunsend = 0;
		//  if(ioctl(sockfd, SIOCOUTQ, (char*)&leftunsend) >= 0)
		{
			if (leftunsend > 0) {
				struct timeval stTimeout = {0};
				stTimeout.tv_sec		 = 0;
				stTimeout.tv_usec		 = 10 * 1000;
				select(0, 0, 0, 0, &stTimeout);
			} else {
				// DEBUGP("=====ccccccc====\n");
				close(sockfd);
				sockfd = -1;
				// DEBUGP("=====dddddddd====\n");
				res = TRUE;
				break;
			}
		}
	}
	return (res);
}
static BOOL remotesocket_tcpclient_del(REMOTE_SOCK_CLIENT_PARAM* clients, int pos)
{
	BOOL res = FALSE;
	if (pos < REMOTESOCKET_MAX_SERVERS) {
		pthread_mutex_lock(&clients->client[pos].lock);
		clients->client[pos].connectstate = CLI_STATE_DISCONNECT;
		memset(clients->client[pos].to, 0, MSG_ID_SIZE);

		remotesocket_tcp_close(clients->client[pos].fd);
		clients->client[pos].fd = -1;
		// DEBUGP("%s==========test by mjx =====del tcp client ===\n",
		// app_name);
		pthread_mutex_unlock(&clients->client[pos].lock);
		res = TRUE;
	}
	return res;
}

BOOL remotesocket_socks_send(char* pid, U8* msg, U32 length, U32 contype, BOOL waitack)
{
	if (msg == NULL || length < MSG_SYNC_HEADER_LEN) {
		return FALSE;
	}

	U16 i	  = 0;
	U32 llen   = 0;
	int pos	= -1;
	int sockfd = -1;
	// BOOL					 res	   = FALSE;
	BOOL					 send_flag = FALSE;
	REMOTE_SOCK_CLIENT_NODE* client	= NULL;
	struct sockaddr_in		 addr	  = {0};
	// char					 sun_path[SOCK_PATH_LEN] = {0};
	if ((contype == REMOTESOCKET_SOCKTYPE_UNKNOW) || (contype == REMOTESOCKET_SOCKTYPE_LONGTCP)
		//|| (contype == REMOTESOCKET_SOCKTYPE_TCPSERVER)
		|| (contype == REMOTESOCKET_SOCKTYPE_SHORTTCP)) {
		for (i = 0; i < REMOTESOCKET_MAX_SERVERS; i++) {
			pthread_mutex_lock(&remotesocket_regs.remote_clients.client[i].lock);
			client = &remotesocket_regs.remote_clients.client[i];

			if (client->connectstate == CLI_STATE_CONNECT) {
				if ((client->socketype == REMOTESOCKET_SOCKTYPE_LONGTCP) && (0 == memcmp(client->to, pid, MSG_ID_SIZE))) {
					debug_count = 3;
					write_fd	= client->fd;
					llen		= send(client->fd, msg, length, 0);
					debug_count++;
					//
					if (llen == length) {
						// DEBUGP(" %s ======test by mjx ===found in old
						// TCP=pos:%d=path:%s=type:%d=fd:%d, len:%d,
						// length:%d\n", app_name, pos, addr.sin_path, contype,
						// client->fd, llen, length);
						send_flag = TRUE;
						// res		  = TRUE;
					}
				}
			} else {
				if (pos < 0)
					pos = i;
			}
			pthread_mutex_unlock(&remotesocket_regs.remote_clients.client[i].lock);
		}
		if (FALSE == send_flag) {
			if (pos >= 0) {
				char ipaddr[255];
				U16  tcp_port;
				U16  udp_port;
				remotesocket_findipbypid(pid, ipaddr, &tcp_port, &udp_port);
				// DEBUGP(" %s ======test by mjx ===found in
				// new=pos:%d=path:%s=type:%d=\n", app_name, pos, addr.sun_path,
				// contype);
				pthread_mutex_lock(&remotesocket_regs.remote_clients.client[pos].lock);
				client	 = &remotesocket_regs.remote_clients.client[pos];
				client->fd = socket(PF_INET, SOCK_STREAM, 0);
				if (client->fd > 0) {
					// DEBUGP("%s==========test by mjx =====create client node
					// fd:%d ===\n", app_name, client->fd);
					memset(&client->addr_in, 0, sizeof(client->addr_in));
					client->addr_in.sin_family		= AF_INET;
					client->addr_in.sin_addr.s_addr = inet_addr(ipaddr);
					client->addr_in.sin_port		= htons(tcp_port);
					fcntl(client->fd, F_SETFD, FD_CLOEXEC);
					if (0 == connect(client->fd, (struct sockaddr*) &(client->addr_in), sizeof(struct sockaddr))) {
						client->connectstate = CLI_STATE_CONNECT;
						if (contype == REMOTESOCKET_SOCKTYPE_UNKNOW)
							client->socketype = REMOTESOCKET_SOCKTYPE_SHORTTCP;
						else
							client->socketype = contype;
						memcpy(client->to, pid, MSG_ID_SIZE);
						debug_count = 5;
						llen		= send(client->fd, msg, length, 0);
						debug_count++;
						//

						if (llen == length) {
							DEBUGP(" %s ======test by mjx ===remote found in new "
								   "TCP connect pid:%s contype:%d state:%d "
								   "==write:%s =fd:%d len:%d=send_flag:%d =====\n",
								   app_name, pid, contype, client->connectstate, msg + MSG_SYNC_HEADER_LEN, client->fd, length, llen);
							send_flag = TRUE;
							// res		  = TRUE;
							if (contype == REMOTESOCKET_SOCKTYPE_UNKNOW)
								contype = REMOTESOCKET_SOCKTYPE_SHORTTCP;
						}
					} else {
						bzero(&client->addr_in, sizeof(client->addr_in));
						close(client->fd);
						client->fd = -1;
					}
				}
				pthread_mutex_unlock(&remotesocket_regs.remote_clients.client[pos].lock);

				if (send_flag == TRUE && waitack == FALSE && contype == REMOTESOCKET_SOCKTYPE_SHORTTCP) {
					// DEBUGP("%s ==========test by mjx =====del client
					// 222=pos:%d==\n", app_name, pos);
					remotesocket_tcpclient_del(&remotesocket_regs.remote_clients, pos);
				}
			}
		}
	}
	DEBUGP("start to send 4\n");
	if ((send_flag == FALSE) && ((contype == REMOTESOCKET_SOCKTYPE_UNKNOW) || (contype == REMOTESOCKET_SOCKTYPE_UDP))) {
		DEBUGP("start to send 5\n");
		char ipaddr[255];
		U16  tcp_port;
		U16  udp_port;
		if (remotesocket_findipbypid(pid, ipaddr, &tcp_port, &udp_port)) {
			// DEBUGP("start to send 6, %s, %d,\n", ipaddr, tcp_port);
			bzero(&addr, sizeof(addr));
			addr.sin_family		 = AF_INET;
			addr.sin_addr.s_addr = inet_addr(ipaddr); //???ﲻһ??
			addr.sin_port		 = htons(udp_port);
			sockfd				 = socket(PF_INET, SOCK_DGRAM, 0);
			llen				 = sendto(sockfd, msg, length, 0, (struct sockaddr*) &addr, sizeof(struct sockaddr_in));
			// DEBUGP("start to send 6, %s, %d, len=%d,sockfd=%d,\n", ipaddr,
			// tcp_port, llen,sockfd);
			if (llen == length) {
				DEBUGP(" %s ======test by mjx ===found in new UDP contype:%d==\n", app_name, contype);
				// res		  = TRUE;
				send_flag = TRUE;
				contype   = REMOTESOCKET_SOCKTYPE_UDP;
				//
			}
			close(sockfd);
			sockfd = -1;
		}
	}
	// DEBUGP("%s ==========test by mjx ===send_flag %d ==\n", app_name,
	// send_flag);
	return send_flag;
}

static BOOL remotesocket_tcpserver_send(char* pid, U8* msg, int length)
{
	if ((msg == NULL) || (length < MSG_SYNC_HEADER_LEN)) {
		return FALSE;
	}

	int  i   = 0;
	BOOL res = FALSE;
	for (i = 0; i < REMOTESOCKET_MAX_CLIENTS; i++) {
		pthread_mutex_lock(&remotesocket_regs.remote_server.client[i].lock);
		// DEBUGP("send by tcp server connectstate=%d, send pid=%c, memory
		// pid=%d\n", remotesocket_regs.remote_server.client[i].connectstate,
		// pid,remotesocket_regs.remote_server.client[i].from);
		if (remotesocket_regs.remote_server.client[i].connectstate == CLI_STATE_CONNECT) {
			if ((0 == (memcmp(pid, remotesocket_regs.remote_server.client[i].from, MSG_ID_SIZE))) && (remotesocket_regs.remote_server.client[i].fd > 0)) {
				debug_count = 1;
				int len		= send(remotesocket_regs.remote_server.client[i].fd, msg, length, 0);
				DEBUGP("send message success msglen=%d\n", len);
				debug_count = 2;
				if (len == length) {
					res = TRUE;
				}
				// DEBUGP("======test by mjx ===found in old
				// TCP(server)==fd:%d====\n",
				// remotesocket_regs.remote_server.client[i].fd);
				pthread_mutex_unlock(&remotesocket_regs.remote_server.client[i].lock);
				break;
			}
		}
		pthread_mutex_unlock(&remotesocket_regs.remote_server.client[i].lock);
	}
	return (res);
}

BOOL remotesocket_send_bysocktype(char* pid, U8* msg, U32 len, int socktype, BOOL waitack)
{
	BOOL res = FALSE;
	DEBUGP("start to send message 3 %d\n", socktype);
	if (socktype == REMOTESOCKET_SOCKTYPE_TCPSERVER || socktype == REMOTESOCKET_SOCKTYPE_SHORTTCP || socktype == REMOTESOCKET_SOCKTYPE_LONGTCP) {
		DEBUGP("send by tcpserver\n");
		res = remotesocket_tcpserver_send(pid, msg, len);
	}
	if (res == FALSE) {
		res = remotesocket_socks_send(pid, msg, len, socktype, waitack);
	}
	return (res);
}

BOOL remotesocket_send_process(char* pid, U8* msg, U32 len, BOOL waitack, int socktype, BOOL fixedsocketype, BOOL newmsgid, U32 msgid)
{
	U8*  buff = NULL;
	U32  llen = 0;
	BOOL res  = FALSE;
	BOOL req  = TRUE;
	// int socktypes[REMOTESOCKET_SOCKTYPE_NUM] = {0};
	// int type = -1;
	// U32 i = 0;
	// U32						   socknum = 0;
	U32						   lmsgid = 0;
	REMOTESOCKETSendQueueNode* node   = NULL;

	// socknum = 0;
	/*if((socktype >= 0)
			&& (socktype < REMOTESOCKET_SOCKTYPE_NUM))
	{
			socktypes[socknum] = socktype;
			socknum ++;
	}
	if(fixedsocketype == FALSE){
			for(i = 0; i< sizeof(rmtsocktypeorder)/sizeof(int); i++)
			{
					if(socknum < REMOTESOCKET_SOCKTYPE_NUM)
					{
							if(socktype != rmtsocktypeorder[i])
							{
									socktypes[socknum] = rmtsocktypeorder[i];
									socknum ++;
							}
					}
			}
	}
	if(socknum == 0)
	{
			return(FALSE);
	}
	*/
	if (TRUE == waitack) {
		if (remotesocket_sendRegs.count >= REMOTESOCKET_SEND_QUEUE_SIZE) {
			req = FALSE;
		}
	}
	pthread_mutex_lock(&remotesocket_sendRegs.lock);
	if (req == TRUE) {
		if (TRUE == newmsgid) {
			lmsgid = (remotesocket_sendRegs.counter & 0x7FFFFFFF);
			remotesocket_sendRegs.counter++;

		} else {
			lmsgid = msgid;
		}
		if (msg != NULL && len > 0) {
			llen = len + MSG_SYNC_HEADER_LEN;
			buff = malloc(llen);
			memset(buff, 0, llen);
			memcpy(buff + MSG_SYNC_HEADER_LEN, msg, len);
		} else {
			pthread_mutex_unlock(&remotesocket_sendRegs.lock);
			return (FALSE);
		}
		remotesocket_send_addheader(buff, lmsgid);
		DEBUGP("start to send message 2\n");
		// for(i = 0; i< socknum; i++)
		{
			res = remotesocket_send_bysocktype(pid, buff, llen, socktype, waitack);
			/*if(TRUE == res)
			{
					type = socktypes[i];
					break;
			}*/
		}
	}
	if (TRUE == res) {
		if (waitack) {
			if (remotesocket_sendRegs.inp >= REMOTESOCKET_SEND_QUEUE_SIZE) {
				remotesocket_sendRegs.inp = 0;
			}
			node		= &remotesocket_sendRegs.queue[remotesocket_sendRegs.inp];
			node->msgid = lmsgid;
			memcpy(node->pid, pid, MSG_ID_SIZE);
			node->waitack = waitack;
			remotesocket_sendRegs.inp++;
			remotesocket_sendRegs.count++;
			remotesocket_sendRegs.counter++;
		}
	}
	if (buff != NULL) {
		free(buff);
		buff = NULL;
	}
	pthread_mutex_unlock(&remotesocket_sendRegs.lock);
	return (res);
}
BOOL remotesocket_udpserver_init(REMOTESOCKET_CALLBACK_FUN callback, const char* sun_path, int port)
{
	if (sun_path == NULL || callback == NULL) {
		return FALSE;
	}

	BOOL res = -1;
	// int  i	 = 0;
	int reuse = 1;
	unlink(sun_path);
	memset(&remotesocket_regs.udp_server, 0, sizeof(REMOTE_SOCK_SERVER_PARAM));
	bzero(&remotesocket_regs.udp_server.addr_in, sizeof(remotesocket_regs.udp_server.addr_in));
	remotesocket_regs.udp_server.sockfd					 = socket(AF_INET, SOCK_DGRAM, 0);
	remotesocket_regs.udp_server.addr_in.sin_family		 = AF_INET;
	remotesocket_regs.udp_server.addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
	remotesocket_regs.udp_server.addr_in.sin_port		 = htons(port);

	fcntl(remotesocket_regs.udp_server.sockfd, F_SETFD, FD_CLOEXEC);
	if (remotesocket_regs.udp_server.sockfd <= 0) {
		return FALSE;
	}

	if (setsockopt(remotesocket_regs.udp_server.sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)) < 0) {
		DEBUGP("%s:%d set socket option failed\n", __FUNCTION__, __LINE__);
	}
	res = bind(remotesocket_regs.udp_server.sockfd, (struct sockaddr*) &remotesocket_regs.udp_server.addr_in, sizeof(remotesocket_regs.udp_server.addr_in));
	if (res) {
		DEBUGP("Bind error\n server is already running\n");
		close(remotesocket_regs.udp_server.sockfd);
		return FALSE;
	}
	pthread_mutex_init(&remotesocket_regs.udp_server.lock, NULL);
	pthread_mutex_init(&remotesocket_regs.udp_server.client.lock, NULL);
	remotesocket_regs.udp_server.callback = callback;
	DEBUGP("success to init udp server, fd=%d, port=%d\n", remotesocket_regs.udp_server.sockfd, port);
	usleep(100);
	return TRUE;
}
BOOL remotesocket_tcpserver_init(REMOTESOCKET_CALLBACK_FUN callback, const char* sun_path, int port)
{
	if (sun_path == NULL || callback == NULL) {
		return FALSE;
	}

	BOOL res   = -1;
	int  i	 = 0;
	int  reuse = 1;
	unlink(sun_path);
	memset(&remotesocket_regs.remote_server, 0, sizeof(REMOTE_SOCK_SERVER_PARAM));
	remotesocket_regs.remote_server.sockfd					= socket(AF_INET, SOCK_STREAM, 0);
	remotesocket_regs.remote_server.addr_in.sin_family		= AF_INET;
	remotesocket_regs.remote_server.addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
	// if(g_test_flag == 0)
	remotesocket_regs.remote_server.addr_in.sin_port = htons(port);
	// else
	// remotesocket_regs.remote_server.addr_in.sin_port = htons(port+1);
	DEBUGP("fadfafasfdsafsafsafad port=%d\n", port);
	fcntl(remotesocket_regs.remote_server.sockfd, F_SETFD, FD_CLOEXEC);
	if (remotesocket_regs.remote_server.sockfd <= 0) {
		return FALSE;
	}

	if (setsockopt(remotesocket_regs.remote_server.sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)) < 0) {
		DEBUGP("%s:%d set socket option failed\n", __FUNCTION__, __LINE__);
	}
	res = bind(remotesocket_regs.remote_server.sockfd, (struct sockaddr*) &remotesocket_regs.remote_server.addr_in,
			   sizeof(remotesocket_regs.remote_server.addr_in));
	if (res) {
		DEBUGP("Bind error\n server is already running\n");
		close(remotesocket_regs.remote_server.sockfd);
		return FALSE;
	}
	pthread_mutex_init(&remotesocket_regs.remote_server.lock, NULL);
	for (i = 0; i < REMOTESOCKET_MAX_CLIENTS; i++) {
		pthread_mutex_init(&remotesocket_regs.remote_server.client[i].lock, NULL);
	}
	remotesocket_regs.remote_server.callback = callback;
	DEBUGP("success to init tcp server\n");
	usleep(100);
	return TRUE;
}

BOOL remotesocket_init(CONFIG_PARAM* config, REMOTESOCKET_CALLBACK_FUN callback)
{
	U32  i						 = 0;
	BOOL res					 = TRUE;
	char sun_path[SOCK_PATH_LEN] = {0};
	DEBUGP("fadfafa 1, %d, %p\n", config->condpidnum, config);
	BOOL enabletcpsrv = config->enabletcpsrv;
	BOOL enableudpsrv = config->enableudpsrv;

	char ipaddr[255];
	U16  tcp_port;
	U16  udp_port;
	remotesocket_findipbypid(config->pid, ipaddr, &tcp_port, &udp_port);

	memcpy(remotesocket_regs.pid, config->pid, MSG_ID_SIZE);
	remotesocket_regs.conpidnum = config->condpidnum;
	if (remotesocket_regs.conpidnum > REMOTESOCKET_MAX_DST_PID) {
		remotesocket_regs.conpidnum = REMOTESOCKET_MAX_DST_PID;
	}
	for (i = 0; i < remotesocket_regs.conpidnum; i++) {
		DEBUGP("fadfafa 3, i= %d, num = %d\n", i, remotesocket_regs.conpidnum);
		remotesocket_regs.conpid[i].contype = config->condpid[i].contype;
		memcpy(remotesocket_regs.conpid[i].pid, config->condpid[i].pid, MSG_ID_SIZE);
	}
	DEBUGP("fadfafa 2\n");

	remotesocket_regs.enabletcpsrv = FALSE;
	remotesocket_regs.enableudpsrv = FALSE;

	if (TRUE == enabletcpsrv) {
		if (TRUE == remotesocket_tcpserver_init(callback, sun_path, tcp_port)) {
			remotesocket_regs.enabletcpsrv = TRUE;
		}
	}
	if (TRUE == enableudpsrv) {
		if (TRUE == remotesocket_udpserver_init(callback, sun_path, udp_port)) {
			remotesocket_regs.enableudpsrv = TRUE;
		}
	}
	remotesocket_clients_init(callback);
	DEBUGP("fadfafa 3\n");

	if (remotesocket_regs.enabletcpsrv != enabletcpsrv) {
		res = FALSE;
	}
	if (res == TRUE) {
		if (pthread_create(&remotesocket_regs.remotesocket_receive_thread, NULL, remotesocket_listen, 0)) {
			DEBUGP("remotesocket pthread_create failed\n");
			res = TRUE;
		}
	}

	if (pthread_create(&remotesocket_regs.remotesocket_timer_thread, NULL, remotesocket_time, 0)) {
		// DEBUGP("remotesocket pthread_create failed\n");
		res = TRUE;
	}

	DEBUGP("fadfsafasf\n");
	return res;
}

BOOL remotesocket_send(char* pid, U8* msg, U32 len, BOOL waitack)
{
	BOOL res	  = FALSE;
	int  socktype = 0;
	socktype	  = remotesocket_findsocktypebypid(pid);
	DEBUGP("start to send message 1\n");
	res = remotesocket_send_process(pid, msg, len, waitack, socktype, FALSE, TRUE, 0);
	return (res);
}
BOOL remotesocket_foward(char* pid, U8* msg, U32 len, U32 msgid)
{
	BOOL res	  = FALSE;
	int  socktype = 0;
	socktype	  = remotesocket_findsocktypebypid(pid);
	DEBUGP("start to foward message %s, %d\n", pid, socktype);
	res = remotesocket_send_process(pid, msg, len, FALSE, socktype, FALSE, FALSE, msgid);
	return (res);
}

BOOL remotesocket_sendack(char* pid, U8* msg, U32 len, U32 msgid)
{
	BOOL res	  = FALSE;
	int  socktype = 0;
	DEBUGP("Ryan tset....start send sock ack...\n");
	socktype = remotesocket_findsocktypebypid(pid);
	// DEBUGP("start to send ack %s, %d\n", pid, socktype);
	msgid = msgid | 0x80000000;
	res   = remotesocket_send_process(pid, msg, len, FALSE, socktype, FALSE, FALSE, msgid);
	return (res);
}

static int remotesocket_tcpserver_add(REMOTE_SOCK_SERVER_PARAM* server, int fd, struct sockaddr_in* addr)
{
	int i	  = 0;
	int res	= -1;
	int emptyi = -1;
	int oldi   = -1;
	int pos	= -1;

	for (i = 0; i < REMOTESOCKET_MAX_CLIENTS; i++) {
		if (server->client[i].connectstate == CLI_STATE_DISCONNECT) {
			if (emptyi < 0) {
				emptyi = i;
			}
		} else {
			if (fd == server->client[i].fd) {
				oldi = i;
			}
		}
	}
	if (oldi >= 0) {
		pos = oldi;
	} else {
		if (emptyi >= 0) {
			pos = emptyi;
		}
	}
	// DEBUGP("%s add to client %d\n", app_name, pos);
	if (pos >= 0) {
		if (fd > 0)
			FD_SET(fd, &server->readfds);
		pthread_mutex_lock(&server->client[pos].lock);
		DEBUGP("add to client %d\n", fd);
		server->client[pos].fd = fd;
		memcpy(&server->client[pos].addr_in, addr, sizeof(struct sockaddr_in));
		server->client[pos].connectstate = CLI_STATE_CONNECT;
		pthread_mutex_unlock(&server->client[pos].lock);
		res = pos;
	}
	return res;
}

static BOOL remotesocket_tcpserver_del(REMOTE_SOCK_SERVER_PARAM* server, int fd)
{
	if (fd < 0)
		return FALSE;

	int  i   = 0;
	BOOL res = FALSE;
	for (i = 0; i < REMOTESOCKET_MAX_CLIENTS; i++) {
		if (server->client[i].fd == fd) {
			FD_CLR(fd, &server->readfds);
			pthread_mutex_lock(&server->client[i].lock);
			server->client[i].connectstate = CLI_STATE_DISCONNECT;
			memset(server->client[i].from, 0, MSG_ID_SIZE);
			memset(server->client[i].to, 0, MSG_ID_SIZE);
			// DEBUGP("=====aaaaaa====\n");
			close(server->client[i].fd);
			// DEBUGP("=====bbbbbb====\n");
			server->client[i].fd = -1;
			// DEBUGP("%s==========test by mjx =====del tcp server ===\n",
			// app_name);

			DEBUGP("delete from client %d\n", fd);
			pthread_mutex_unlock(&server->client[i].lock);
			res = TRUE;
			// break; //if there is  two same fd.
		}
	}
	return res;
}

static int remotesocket_tcpserver_set(REMOTE_SOCK_SERVER_PARAM* server, int fd, char* pid)
{
	int i   = 0;
	int res = -1;
	for (i = 0; i < REMOTESOCKET_MAX_CLIENTS; i++) {
		if (server->client[i].connectstate == CLI_STATE_CONNECT) {
			if (fd == server->client[i].fd) {
				pthread_mutex_lock(&server->client[i].lock);
				memcpy(server->client[i].from, pid, MSG_ID_SIZE);
				DEBUGP("add to client %d, pid=%s\n", fd, pid);
				pthread_mutex_unlock(&server->client[i].lock);
				res = i;
				break;
			}
		}
	}
	return (res);
}

#if 0
static int remotesocket_tcpserver_search(REMOTE_SOCK_SERVER_PARAM* server,
										 char*					   pid)
{
	int i   = 0;
	int res = -1;
	for (i = 0; i < REMOTESOCKET_MAX_CLIENTS; i++) {
		if (server->client[i].connectstate == CLI_STATE_CONNECT) {
			if (0 == memcmp(pid, server->client[i].from, MSG_ID_SIZE)) {
				res = i;
				break;
			}
		}
	}
	return (res);
}
#endif

//#define REMOTESOCKET_TCP_BUFF_SIZE 10240
#define REMOTESOCKET_TCP_BUFF_SIZE 60720
#define REMOTESOCKET_TCP_STATE_HEADER 0
#define REMOTESOCKET_TCP_STATE_MALLOC 1
#define REMOTESOCKET_TCP_STATE_PAYLOAD 2
#define REMOTESOCKET_TCP_STATE_FINISH 3

static BOOL remotesocket_receive(int sockfd, RMTSOCK_RECMSG_STATE* recstate, REMOTESOCKET_CALLBACK_FUN callback)
{
	if (recstate == NULL) {
		return FALSE;
	}
	U32  i									 = 0;
	U32  recvlength							 = 0;
	U32  msglength							 = 0;
	U32  len								 = 0;
	U8   recvbuf[REMOTESOCKET_TCP_BUFF_SIZE] = {0};
	U32  bytes								 = REMOTESOCKET_TCP_BUFF_SIZE;
	BOOL res								 = FALSE;
	if (bytes > 0) {
		bytes = read(sockfd, recvbuf, REMOTESOCKET_TCP_BUFF_SIZE);
		// DEBUGP("%s=========test by mjx recv buf:%s =======%d\n", app_name,
		// recvbuf + SOCKET_HEADER_LEN,recstate->state);
		for (i = 0; i < bytes; i++) {
			// DEBUGP("%02x", recvbuf[i]);
		}
		DEBUGP("\n");
		i = 0;
		while (i < bytes) {
			if (recstate->state == REMOTESOCKET_TCP_STATE_HEADER) {
				if (recstate->headerpointer >= SOCKET_HEADER_LEN) {
					recstate->headerpointer = 0;
				}
				recstate->headerring[recstate->headerpointer] = recvbuf[i];
				recstate->headerpointer++;
				recstate->headercount++;
				i++;
				if (recstate->headercount >= SOCKET_HEADER_LEN) {
					if (TRUE == checkheader(&recstate->sock_header, recstate->headerring, recstate->headerpointer)) {
						recstate->state = REMOTESOCKET_TCP_STATE_MALLOC;
						// DEBUGP("goto fadfaa\n");
					}
				}
			}
			if (recstate->state == REMOTESOCKET_TCP_STATE_MALLOC) {
				if (recstate->recv_buff != NULL) {
					DEBUGP("=======test by mjx ==test recv finish 3333====%p==\n", recstate->recv_buff);
					free((void*) recstate->recv_buff);
					DEBUGP("=======test by mjx ==test recv finish 4444====%p==\n", recstate->recv_buff);
					recstate->recv_buff = NULL;
				}
				recstate->datalen = MAKEFOURCC_BE4(recstate->sock_header.msgHeader.dataLen[0], recstate->sock_header.msgHeader.dataLen[1],
												   recstate->sock_header.msgHeader.dataLen[2], recstate->sock_header.msgHeader.dataLen[3]);

				recstate->msgid = MAKEFOURCC_BE4(recstate->sock_header.syncHeader.msgid.bytes[0], recstate->sock_header.syncHeader.msgid.bytes[1],
												 recstate->sock_header.syncHeader.msgid.bytes[2], recstate->sock_header.syncHeader.msgid.bytes[3]);
				// DEBUGP("=========test by zzyrecv buf:%d,msgid=%d,i=%d,
				// length=%04x=======\n", recstate->datalen,recstate->msgid, i,
				// bytes);
				if (recstate->datalen > 0) // add by mjx. whether to take a limit of the max size
					recstate->recv_buff = (U8*) malloc(recstate->datalen + 1);
				if (recstate->recv_buff != NULL)
					memcpy(recstate->recv_buff, (U8*) &recstate->sock_header.msgHeader, MSG_HEADER_LENTH);
				recstate->recvcount = MSG_HEADER_LENTH;
				recstate->state		= REMOTESOCKET_TCP_STATE_PAYLOAD;
			}
			if (recstate->state == REMOTESOCKET_TCP_STATE_PAYLOAD) {
				// DEBUGP("=========test by zzy reccount,
				// reclen:%d,%d=======\n", recstate->recvcount,
				// recstate->datalen);

				if (recstate->recvcount >= recstate->datalen) {
					recstate->state = REMOTESOCKET_TCP_STATE_FINISH;
				} else {
					msglength = 0;
					if (bytes > i) {
						msglength = bytes - i;
					}
					if (msglength > 0) {
						recvlength = recstate->datalen - recstate->recvcount;
						if (msglength >= recvlength) {
							len = recvlength;
						} else {
							len = msglength;
						}
						if (recstate->recv_buff != NULL)
							memcpy(&recstate->recv_buff[recstate->recvcount], &recvbuf[i], len);
						i += len;
						recstate->recvcount += len;
						// DEBUGP("=========test by zzy reccount, reclen:%d,%d,
						// len=%d=======\n", recstate->recvcount,
						// recstate->datalen,len);
						if (recstate->recvcount >= recstate->datalen) {
							recstate->recv_buff[recstate->datalen] = 0;
							recstate->state						   = REMOTESOCKET_TCP_STATE_FINISH;
						}
					}
				}
			}
			if (recstate->state == REMOTESOCKET_TCP_STATE_FINISH) {
				recstate->headerpointer = 0;
				recstate->headercount   = 0;

				// callback//

				if (recstate->recv_buff != NULL) {
					DEBUGP("=======test by mjx ==test recv finish======\n");
					remotesocket_tcpserver_set(&remotesocket_regs.remote_server, sockfd, (char*) &recstate->sock_header.msgHeader.srcID[0]);
					DEBUGP("=======test by mjx ==test recv callback:%p buff:%s "
						   "count:%d msgid:%d======\n",
						   callback, recstate->recv_buff + 20, recstate->recvcount, recstate->msgid);
					callback(recstate->recv_buff, recstate->recvcount, recstate->msgid);
					DEBUGP("=======test by mjx ==test recv finish 1111====%p==\n", recstate->recv_buff);
					free(recstate->recv_buff);
					DEBUGP("=======test by mjx ==test recv finish 2222====%p==\n", recstate->recv_buff);
					recstate->recv_buff = NULL;
					recstate->recvcount = 0;
					res					= TRUE;
				}
				recstate->state = REMOTESOCKET_TCP_STATE_HEADER;
			}
		}
	}

	return (res);
}

// static U32 err_count = 0;

static void* remotesocket_time(void* thread)
{
	// int i	 = 0;
	// int bytes = 0;
	int wait = 0;
	while (1) {
		if (wait > 10) {
			// char recvbuf[REMOTESOCKET_TCP_BUFF_SIZE];
			if (write_fd >= 0) {
				// DEBUGP("=====read begin ====\n");
				// read(write_fd, recvbuf, REMOTESOCKET_TCP_BUFF_SIZE);

				/*
				remotesocket_tcpserver_del(&remotesocket_regs.remote_server,
				write_fd);
				for(i = 0; i< REMOTESOCKET_MAX_SERVERS; i++)
				{
						if(remotesocket_regs.remote_clients.client[i].fd ==
				write_fd)
						{
								remotesocket_tcpclient_del(&remotesocket_regs.remote_clients,
				i);
								break;
						}
				}
				*/
				// DEBUGP("=====read end ====\n");
				wait = 0;
			}
		}
		if (debug_count == 3) {
			wait++;
			// DEBUGP("=====%s test by mjx debug count = %d ===========\n",
			// app_name,debug_count);
		} else
			wait = 0;

		sleep(2);
	}
	return NULL;
}

static void* remotesocket_listen(void* thread)
{
	U32  i								   = 0;
	int  fd								   = -1;
	BOOL res							   = -1;
	int  len							   = 0;
	U16 __attribute__((unused)) counter	= 0;
	int						  nread		   = 0;
	BOOL					  enabletcpsrv = 0, enableudpsrv = 0;
	REMOTE_SOCK_SERVER_PARAM* tcpserver = NULL;
	SOCK_UDP_PARAM*			  udpserver = NULL;
	fd_set					  set;
	struct timeval			  tm;
	struct sockaddr_in		  address;

	memset(&set, 0, sizeof(set));
	memset(&tm, 0, sizeof(tm));
	memset(&address, 0, sizeof(address));

	tcpserver	= &remotesocket_regs.remote_server;
	udpserver	= &remotesocket_regs.udp_server;
	enabletcpsrv = remotesocket_regs.enabletcpsrv;
	enableudpsrv = remotesocket_regs.enableudpsrv;
	if (TRUE == enabletcpsrv && tcpserver->sockfd > 0) {
		listen(tcpserver->sockfd, 5);
		FD_ZERO(&tcpserver->readfds);
		FD_SET(tcpserver->sockfd, &tcpserver->readfds);
	}

	DEBUGP("zzy server socket TCP fd=%d\n", remotesocket_regs.remote_server.sockfd);
	while (1) {
		DEBUGP("zzy server socket TCP fd=%d, UDP fd=%d, counter= %d\n", remotesocket_regs.remote_server.sockfd, udpserver->sockfd, counter);
		counter++;
		FD_ZERO(&set);
		if (TRUE == enabletcpsrv) {
			set = tcpserver->readfds;
		}
		if (TRUE == enableudpsrv && udpserver->sockfd) {
			FD_SET(udpserver->sockfd, &set);
		}

		for (i = 0; i < REMOTESOCKET_MAX_SERVERS; i++) {
			if (remotesocket_regs.remote_clients.client[i].fd > 0 && remotesocket_regs.remote_clients.client[i].connectstate == CLI_STATE_CONNECT) {
				// DEBUGP("\n\n%s==========test by mjx =====add client node
				// fd:%d ===\n", app_name,
				// remotesocket_regs.remote_clients.client[i].fd);
				FD_SET(remotesocket_regs.remote_clients.client[i].fd, &set);
			}
		}

		tm.tv_sec  = 1;
		tm.tv_usec = 0;
		DEBUGP("ryan test 111111111\n");
		res = select(FD_SETSIZE, &set, NULL, NULL, &tm); // modify by mjx for no timeout wait
		DEBUGP("ryan test 2222222\n");
		if (res > 0) {
			if (TRUE == enabletcpsrv) {
				if (FD_ISSET(tcpserver->sockfd, &set)) {
					FD_CLR(tcpserver->sockfd, &set);
					len = sizeof(address);
					fd  = accept(tcpserver->sockfd, (struct sockaddr*) &address, (socklen_t*) &len);
					remotesocket_tcpserver_add(tcpserver, fd, &address);
					DEBUGP("%s tcp communication connect fd=%d\n", app_name, fd);
				}
				DEBUGP("ryan test 33333333\n");
				for (i = 0; i < REMOTESOCKET_MAX_CLIENTS; i++) {
					if (tcpserver->client[i].fd > 0 && FD_ISSET(tcpserver->client[i].fd, &set)) {
						ioctl(tcpserver->client[i].fd, FIONREAD, &nread);
						if (nread == 0) {
							DEBUGP("%s tcp fd:%d communication disconnection\n", app_name, tcpserver->client[i].fd);
							remotesocket_tcpserver_del(tcpserver, tcpserver->client[i].fd);
						} else {
							(void) remotesocket_receive(tcpserver->client[i].fd, &tcpserver->client[i].recstate, tcpserver->callback);
						}
					}
				}
				DEBUGP("ryan test 4444444444\n");
			}
			if (TRUE == enableudpsrv) {
				if (FD_ISSET(udpserver->sockfd, &set)) {
					(void) remotesocket_receive(udpserver->sockfd, &udpserver->client.recstate, udpserver->callback);
				}
			}

			for (i = 0; i < REMOTESOCKET_MAX_SERVERS; i++) {
				if (remotesocket_regs.remote_clients.client[i].fd > 0 && FD_ISSET(remotesocket_regs.remote_clients.client[i].fd, &set)) {
					DEBUGP("%s==========test by mjx =====client node recv ===\n", app_name);
					ioctl(remotesocket_regs.remote_clients.client[i].fd, FIONREAD, &nread);
					if (nread == 0) {
						remotesocket_tcpclient_del(&remotesocket_regs.remote_clients, i);
					} else {
						res = remotesocket_receive(remotesocket_regs.remote_clients.client[i].fd, &remotesocket_regs.remote_clients.client[i].recstate,
												   remotesocket_regs.remote_clients.callback);
						if (TRUE == res) {
							if (remotesocket_regs.remote_clients.client[i].socketype != REMOTESOCKET_SOCKTYPE_LONGTCP) {
								remotesocket_tcpclient_del(&remotesocket_regs.remote_clients, i);
							}
						}
					}
				}
			}
		}
	}
	DEBUGP("ryan test....finish...\n");
	return NULL;
}

void remotesocket_clients_init(REMOTESOCKET_CALLBACK_FUN callback)
{
	U32 i = 0;
	memset(&remotesocket_regs.remote_clients, 0, sizeof(REMOTE_SOCK_CLIENT_PARAM));
	pthread_mutex_init(&remotesocket_regs.remote_clients.lock, NULL);
	for (i = 0; i < REMOTESOCKET_MAX_SERVERS; i++) {
		pthread_mutex_init(&remotesocket_regs.remote_clients.client[i].lock, NULL);
	}
	remotesocket_regs.remote_clients.callback = callback;
}

void remotesocket_send_addheader(U8* msg, U32 msgid)
{
	U8			   crc[2] = {0};
	SOCKET_HEADER* header = NULL;
	header				  = (SOCKET_HEADER*) msg;
	crc_calc((U8*) &header->msgHeader, MSG_HEADER_LENTH, crc);
	header->syncHeader.headCRC.bytes[0] = 0;
	header->syncHeader.headCRC.bytes[1] = 0;
	header->syncHeader.headCRC.bytes[2] = crc[0];
	header->syncHeader.headCRC.bytes[3] = crc[1];
	header->syncHeader.control			= 0;
	header->syncHeader.msgid.bytes[0]   = (msgid >> 24) & 0xff;
	header->syncHeader.msgid.bytes[1]   = (msgid >> 16) & 0xff;
	header->syncHeader.msgid.bytes[2]   = (msgid >> 8) & 0xff;
	header->syncHeader.msgid.bytes[3]   = (msgid) &0xff;
	header->syncHeader.frameHeader.word = SOCKET_SYNC_HEADER;
}
int remotesocket_findsocktypebypid(char* pid)
{
	U32 i   = 0;
	int res = REMOTESOCKET_SOCKTYPE_UNKNOW;
	for (i = 0; i < REMOTESOCKET_MAX_DST_PID; i++) {
		if (0 == memcmp(remotesocket_regs.conpid[i].pid, pid, MSG_ID_SIZE)) {
			res = remotesocket_regs.conpid[i].contype;
			// DEBUGP("==========test by mjx ======founded type:%d==\n", res);
		}
	}
	return (res);
}
U8 remotesocket_findipbypid(char* pid, char* ip, U16* tcp_port, U16* udp_port)
{
	U32 i;
	U8  res = FALSE;
	for (i = 0; i < sizeof(remote_ipmap) / sizeof(STRREMOTE_IPMAP); i++) {
		if (memcmp(pid, remote_ipmap[i].pid, MSG_ID_SIZE) == 0) {
			strcpy(ip, remote_ipmap[i].ip);
			*tcp_port = remote_ipmap[i].tcp_port;
			*udp_port = remote_ipmap[i].udp_port;
			res		  = TRUE;
			break;
		}
	}
	return (res);
}

BOOL remotesocket_tcp_send(char* pid, U8* msg, U32 len, BOOL waitack)
{
	BOOL res = FALSE;
	res		 = remotesocket_send_process(pid, msg, len, waitack, REMOTESOCKET_SOCKTYPE_SHORTTCP, FALSE, TRUE, 0);
	return (res);
}

BOOL remotesocket_udp_send(char* pid, U8* msg, U32 len, BOOL waitack)
{
	BOOL res = FALSE;
	res		 = remotesocket_send_process(pid, msg, len, waitack, REMOTESOCKET_SOCKTYPE_UDP, FALSE, TRUE, 0);
	return (res);
}

BOOL remotesocket_sendackbysocktype(char* pid, U8* msg, U32 len, U32 msgid, int socktype)
{
	return TRUE;
}

static BOOL __remotesocket_sendmsg(U32* msgid, char* did, char* sid, U8 funcode, U8 operatecode, U8* msg, U32 len)
{
	U8*				 buff;
	BOOL			 res = FALSE;
	MSG_DATA_HEADER* msgHeader;

	DEBUGP("generate msg, %s, %s,%d\n", sid, did, len);
	buff = (U8*) malloc(len + MSG_HEADER_LENTH + 1);
	memset(buff, 0, (len + MSG_HEADER_LENTH + 1));
	buff[len + MSG_HEADER_LENTH] = 0;
	msgHeader					 = (MSG_DATA_HEADER*) buff;
	memcpy(&buff[MSG_HEADER_LENTH], msg, len);
	memcpy(msgHeader->dstID, did, MSG_ID_SIZE);
	memcpy(msgHeader->srcID, sid, MSG_ID_SIZE);

	msgHeader->funcCode   = funcode;
	msgHeader->operCode   = operatecode;
	msgHeader->dataLen[0] = (U8)(((len + MSG_HEADER_LENTH) >> 24) & 0xff);
	msgHeader->dataLen[1] = (U8)(((len + MSG_HEADER_LENTH) >> 16) & 0xff);
	msgHeader->dataLen[2] = (U8)(((len + MSG_HEADER_LENTH) >> 8) & 0xff);
	msgHeader->dataLen[3] = (U8)((len + MSG_HEADER_LENTH) & 0xff);

	if (NULL != msgid) {
		res = remotesocket_sendack(did, buff, len + MSG_HEADER_LENTH, *msgid);
	} else {
		res = remotesocket_send(did, buff, len + MSG_HEADER_LENTH, FALSE);
	}

	free(buff);
	buff = NULL;
	return (res);
}

BOOL remotesocket_sendmsgAck(U32 msgid, char* did, char* sid, U8 funcode, U8 operatecode, U8* msg, U32 len)
{
	U32 tmp_msgid = msgid;
	return __remotesocket_sendmsg(&tmp_msgid, did, sid, funcode, operatecode, msg, len);
}

BOOL remotesocket_sendmsg(char* did, char* sid, U8 funcode, U8 operatecode, U8* msg, U32 len)
{
	return __remotesocket_sendmsg(NULL, did, sid, funcode, operatecode, msg, len);
}
