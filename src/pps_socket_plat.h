#ifndef PPS_SOCKET_PLAT_H
#define PPS_SOCKET_PLAT_H

#include "pps_socket_epoll.h"
#include "pps_socket_win.h"

#ifdef WIN_SOCK_MAX
#define SOCK_CTX_SLOT_NUM WIN_SOCK_MAX+1024
#define MAX_SOCK_CTX WIN_SOCK_MAX
#else
#define SOCK_CTX_SLOT_NUM 65536
#define MAX_SOCK_CTX 65000
#endif // WIN_SOCK_MAX

#endif // !PPS_SOCKET_PLAT_H

