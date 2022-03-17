#ifndef PPS_NET_API_H
#define PPS_NET_API_H
#include "pps_net_task.h"
#include <cstdint>

struct pipes;
#define TASK_REQ_BUF_LEN 128
struct net_task_req
{
	int16_t cmd;
	uint16_t szBuf;
	struct netreq_src src;
	char buf[TASK_REQ_BUF_LEN];
};

int netapi_tcp_listen(struct netreq_src* src, struct pipes* pipes, struct tcp_server_cfg* cfg);
int netapi_tcp_read(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt, struct read_arg* arg);
int netapi_tcp_send(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt, const char* buf, int size);
int netapi_get_remote(struct pipes* pipes, int idx, uint32_t cnt, char* buf, int szBuf, int* port);
int netapi_close_sock(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt);
int netapi_is_listen_valid(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt);

int netapi_tcp_connect(struct netreq_src* src, struct pipes* pipes, struct tcp_connect_cfg* cfg);

#endif // !PPS_NET_API_H

