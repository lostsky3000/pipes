#ifndef PPS_NET_TASK_H
#define PPS_NET_TASK_H

#include <cstdint>

#define NETCMD_TCP_LISTEN 1
#define NETCMD_TCP_CONNIN 2
#define NETCMD_TCP_ADD 3
#define NETCMD_TCP_CLOSE 4
#define NETCMD_READ_WAIT 5
#define NETCMD_SEND_WAIT 6
#define NETCMD_READ_OVER 7
#define NETCMD_TCP_CONNECT 8
#define NETCMD_SHUTDOWN 9

struct netreq_src
{
	int32_t idx;
	uint32_t cnt;
	int session;
};
struct netmsg_src
{
	int32_t idx;
	uint32_t cnt;
};

//
#define SOCK_TCP_LISTEN_ADDR_MAX 8
struct tcp_server_cfg
{
	int port;
	int backlog;
	int sendBuf;
	int recvBuf;
	int addrNum;
	struct protocol_cfg* protocolCfg;
	SOCK_ADDR addrs[SOCK_TCP_LISTEN_ADDR_MAX];
};
struct netreq_tcp_listen
{
	struct netreq_src src;
	struct tcp_server_cfg* cfg;
};
struct netreq_tcp_shutdown
{
	struct netreq_src src;
};
struct tcp_connect_cfg
{
	int sendBuf;
	int recvBuf;
	int timeout;
	int port;
	char* host;
};
struct tcp_conn_info
{
	int64_t expiration;
	int timeout;
	int inProgress;
	int sendbuf;
	int recvbuf;
	int portRemote;
	SOCK_FD fd;
	SOCK_ADDR addrRemote;
};
struct netreq_tcp_conn
{
	struct netreq_src src;
	struct tcp_conn_info* info;
};

struct tcp_add_info
{
	int32_t parentIdx;
	uint32_t parentCnt;
	struct netreq_src src;
};
struct netreq_tcp_add
{
	struct netmsg_src sockId;
	struct tcp_add_info* info;
};
struct netreq_tcp_close
{
	struct netmsg_src sockId;
};

struct tcp_read_wait
{
	int32_t waitReadable;
	struct netmsg_src sockId;
};
struct netreq_read_wait
{
	struct netreq_src src;
	struct tcp_read_wait* wait;
};
struct netreq_send_wait
{
	struct netreq_src src;
};
// =========== 
struct tcp_listen_ret
{
	int ret;
	struct netmsg_src sockId;
};
struct tcp_conn_in
{
	struct netmsg_src sockId;
	struct netmsg_src sockIdParent;
};
struct tcp_read_wait_ret
{
	struct netmsg_src sockId;
	int session;
};
struct tcp_conn_ret
{
	int ret;
	struct netmsg_src sockId;
};

#endif // !PPS_NET_TASK_H

