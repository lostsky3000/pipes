#ifndef PPS_NET_H
#define PPS_NET_H
#include <cstdint>
#include <atomic>
#include <cstring>
#include <unordered_set>
#include "pps_socket.h"
#include "mq_mpsc.h"
#include "pool_linked.h"
#include "pps_net_task.h"
#include "pipes.h"
#include "minheap.h"


typedef int(*CB_SOCK_TCP_ACCEPT)(struct pps_net* net, struct socket_sockctx* sock, 
	SOCK_FD fd, SOCK_ADDR* addrRemote, int portRemote);
typedef int(*CB_SOCK_TCP_READ)(struct pps_net* net, struct socket_sockctx* sock);
typedef int(*CB_SOCK_TCP_SEND)(struct pps_net* net, struct socket_sockctx* sock);
typedef int(*CB_SOCK_TCP_CONN_WAIT)(struct pps_net* net, struct socket_sockctx* sock, int errCode);


#define NET_MSG_BUF_LEN 30
struct net_msg_ori
{
	struct netreq_src to;
	uint8_t cmd;
	uint8_t szBuf;
	char buf[NET_MSG_BUF_LEN];
};
struct net_msg
{
	uint32_t svcCnt;
	int32_t session;
	int16_t cmd;
	uint16_t szBuf;
	char buf[NET_MSG_BUF_LEN+2];
};

struct conn_wait_info
{
	int8_t waitTaskDone;
	int8_t waitTaskSucc;
	int8_t inTimedoutQueue;
	int8_t hasTimedout;
	int timeout;
	int64_t expiration;
	struct socket_ctx* ctx;
};

struct pipes;
struct pps_net 
{
	uint32_t index; 
	int32_t pollWait;
	struct minheap_queue<struct conn_wait_info*>* queConnTimeout;
	int32_t connTimeoutNum;
	//
	struct pipes* pipes;
	struct socket_mgr* sockMgr;
	struct mq_mpsc<struct net_task_req>* queTaskReq;
	//
	struct pool_linked<struct net_msg_ori>* queUnSendMsg;
	uint32_t unSendMsgNum;
	//
	struct pool_linked<int32_t>* queWaitRecycleSock;
	uint32_t waitRecycleSockNum;
	//
	SOCK_EVENT_FD* eventFd;
	struct socket_ctx sockCtxEvent;
	//
	SOCK_POLL_FD* pollFd;
	//
	struct ST_POLL_RUNTIME* pollRuntime;
	//
	CB_SOCK_TCP_ACCEPT cbTcpAccept;
	CB_SOCK_TCP_READ cbTcpRead;
	CB_SOCK_TCP_SEND cbTcpSend;
	CB_SOCK_TCP_CONN_WAIT cbTcpConnWait;
	//
	struct net_msg_ori netMsgOri;
	struct net_msg_ori* pNetMsgOri;
	//
	char bufAddr[SOCK_ADDR_STRLEN];
	//
	std::unordered_set<SOCK_FD>* pSetFd;
};

void net_thread(struct pps_net* net);

void net_shutdown(struct pipes* pipes);


inline void net_wrap_svc_msg(struct net_msg_ori* msg, struct netreq_src* src, int cmd, void*payload, int size)
{
	msg->to = *src;
	msg->cmd = cmd;
	msg->szBuf = size;
	memcpy(msg->buf, payload, size);
}
int net_send_to_svc(struct net_msg_ori* msg, struct pps_net* net, bool addToWait);
int net_send_to_svc_ext(struct net_msg_ori* msg, struct pipes* pipes);

inline struct pps_net* net_alloc(struct pipes* pipes)
{
	uint32_t idxNet = pipes->netAllocCnt.fetch_add(1, std::memory_order_relaxed) % pipes->config->net_num;
	return &pipes->nets[idxNet];
}
//
inline struct pps_net* net_get(struct pipes* pipes, uint32_t idx)
{
	if (pipes->nets) {
		return &pipes->nets[idx];
	}
	return nullptr;
}

inline void net_msg_push_fill(struct net_msg* dst, struct net_msg_ori* src)
{
	dst->cmd = src->cmd;
	dst->session = src->to.session;
	dst->svcCnt = src->to.cnt;
	uint16_t sz = src->szBuf;
	dst->szBuf = sz;
	memcpy(dst->buf, src->buf, sz);
}
inline void net_msg_pop_fill(struct net_msg* src, struct net_msg* dst)
{
	dst->cmd = src->cmd;
	dst->session = src->session;
	dst->svcCnt = src->svcCnt;
	memcpy(dst->buf, src->buf, src->szBuf);
}
//
int net_init_main_thread(struct pps_net* net, struct pipes* pipes, struct socket_mgr* sockMgr, uint32_t index);
void net_deinit_main_thread(struct pps_net* net);

#endif // !PPS_NET_H

