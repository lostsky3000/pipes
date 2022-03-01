#ifndef PPS_SOCKET_WIN_H
#define PPS_SOCKET_WIN_H

#include "pps_macro.h"

#if defined(PLAT_IS_WIN)

#define WIN_SOCK_MAX 1024
#define FD_SETSIZE WIN_SOCK_MAX

#include <WinSock2.h>
#include "pool_linked.h"

typedef SOCKET SOCK_FD;
typedef SOCKET SOCK_EVENT_FD;
typedef struct in_addr SOCK_ADDR;

struct socket_sockctx;
struct poll_ctx
{
	int sockNum;
	struct socket_sockctx* arrSockCtx[WIN_SOCK_MAX];
	struct pool_linked<int>* plkFreeIdx;
	struct pool_linked<int>* plkFreeIdxOri;
	fd_set readSet;
	fd_set sendSet;
	fd_set exceptSet;
	struct timeval tmOut;
};
typedef struct poll_ctx SOCK_POLL_FD;

struct ST_POLL_RUNTIME
{
	int addrLenCli;
	sockaddr_in inAddrCli;
	//char hostBuf[INET_ADDRSTRLEN];
	//struct epoll_event events[EPOLL_EVENTS_MAX];
};

#endif

#endif // !PPS_SOCKET_WIN_H
