#ifndef PPS_SOCKET_EPOLL_H
#define PPS_SOCKET_EPOLL_H

#include "pps_macro.h"

#if defined(PLAT_IS_LINUX)

#include <sys/epoll.h>
#include <arpa/inet.h>
#include <cstdint>

#define EPOLL_EVENTS_MAX 4096


typedef int SOCK_EVENT_FD;
typedef int SOCK_POLL_FD;
typedef int SOCK_FD;
typedef struct in_addr SOCK_ADDR;

struct ST_POLL_RUNTIME
{
	socklen_t addrLenCli;
	struct sockaddr_in inAddrCli;
	char hostBuf[INET_ADDRSTRLEN];
	struct epoll_event events[EPOLL_EVENTS_MAX];
};


#endif


#endif // !PPS_SOCKET_EPOLL_H

