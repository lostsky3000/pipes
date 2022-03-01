#ifndef PPS_SOCK_TASK_REQ_H
#define PPS_SOCK_TASK_REQ_H

#include <cstdint>

struct sockreq_src
{
	int32_t idx;
	uint32_t cnt;
	int session;
};

struct tcp_server_cfg;
struct sockreq_tcp_listen
{
	struct sockreq_src src;
	struct tcp_server_cfg* cfg;
};

struct sockreq_tcp_add
{
	struct sockreq_src src;
	uint32_t idxCtx;
};

#define REQCMD_TCP_LISTEN 1
#define REQCMD_TCP_ADD 2


#endif // !PPS_SOCK_TASK_REQ_H

