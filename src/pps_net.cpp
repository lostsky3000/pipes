#include "pps_net.h"
#include "pipes.h"
#include "pps_socket.h"
#include "pps_socket_define.h"
#include "pps_service.h"
#include "pps_malloc.h"
#include "tcp_protocol.h"
#include <thread>
#include <cassert>

typedef int(*FN_READDEC_READ)(struct read_runtime* run, int* notifyReadable, int* trunc);
typedef int(*FN_READDEC_INIT)(struct read_runtime* run, struct read_arg* arg);
static inline int dec_init_now(struct read_runtime* run, struct read_arg* arg);
static inline int dec_init_len(struct read_runtime* run, struct read_arg* arg);
static inline int dec_init_sep(struct read_runtime* run, struct read_arg* arg);
static inline int dec_init_all(struct read_runtime* run, struct read_arg* arg);
static FN_READDEC_INIT s_arrDecInit[TCP_DEC_NUM] = {dec_init_now, dec_init_len, dec_init_sep, dec_init_all};

static int dec_read_now(struct read_runtime* run, int* waitReadable, int*trunc);
static int dec_read_len(struct read_runtime* run, int* waitReadable, int*trunc);
static int dec_read_sep(struct read_runtime* run, int* waitReadable, int*trunc);
static int dec_read_all(struct read_runtime* run, int* waitReadable, int*trunc);
static FN_READDEC_READ s_arrDecRead[TCP_DEC_NUM] = {dec_read_now, dec_read_len, dec_read_sep, dec_read_all};

//
static struct pps_net* alloc_net(struct pipes* pipes)
{
	uint32_t idxNet = pipes->netAllocCnt.fetch_add(1, std::memory_order_relaxed) % pipes->config->net_num;
	return &pipes->nets[idxNet];
}
template<typename T>
static void send_to_net(struct pps_net*netDst, T*ud, void(*fnFill)(struct net_task_req*, T*), struct pps_net*netSrc)
{
	if (!mpsc_push_ptr_custom(netDst->queTaskReq, ud, fnFill)) {
		while (!mpsc_push_ptr_custom(netDst->queTaskReq, ud, fnFill)) {}
	}
	if (netSrc == nullptr || netSrc->index != netDst->index) {
		sock_event_notify(netDst->eventFd);
	}
}

static inline void wrap_svc_msg(struct net_msg_ori* msg, struct netreq_src* src, int cmd, void*payload, int size)
{
	msg->to = *src;
	msg->cmd = cmd;
	msg->szBuf = size;
	memcpy(msg->buf, payload, size);
}
static int send_to_svc(struct net_msg_ori* msg, struct pps_net* net, bool addToWait);
static void fn_pop_net_req(struct net_task_req* src, struct net_task_req* dst);
static void send_readnotify_to_svc(struct pps_net* net, struct netreq_src* src, struct tcp_read_wait_ret* ret)
{
	wrap_svc_msg(net->pNetMsgOri, src, NETCMD_READ_WAIT, ret, sizeof(struct tcp_read_wait_ret));
	send_to_svc(net->pNetMsgOri, net, true);	
}
static inline int poll_evt_add(SOCK_POLL_FD* pollFd, struct socket_sockctx* sockCtx) 
{
	struct socket_ctx* ctx = sockCtx->main;
	uint32_t events = 0;
	if (ctx->pollInReg) {
		events |= SOCK_POLLEVT_IN;
	}
	if (ctx->pollOutReg) {
		events |= SOCK_POLLEVT_OUT;
	}
	return sock_poll_add_fd(pollFd, sockCtx, events);
}
static inline int poll_evt_mod(SOCK_POLL_FD* pollFd, struct socket_sockctx* sockCtx) 
{
	struct socket_ctx* ctx = sockCtx->main;
	uint32_t events = 0;
	if (ctx->pollInReg) {
		events |= SOCK_POLLEVT_IN;
	}
	if (ctx->pollOutReg) {
		events |= SOCK_POLLEVT_OUT;
	}
	return sock_poll_mod_fd(pollFd, sockCtx, events);
}
static inline int add_net_fd(struct pps_net* net, SOCK_FD* pFd)
{
	SOCK_FD fd = *pFd;
	net->pSetFd->insert(fd);
	return 1;
}
static inline int del_net_fd(struct pps_net* net, SOCK_FD* pFd)
{
	SOCK_FD fd = *pFd;
	net->pSetFd->erase(fd);
	return 1;
}
static inline void recycle_ctx(struct pps_net* net, struct socket_ctx* ctx) {
	uint32_t preCnt = ctx->cnt.fetch_add(1, std::memory_order_relaxed);
	printf("recycle_ctx(), idx=%d, preCnt=%d\n", ctx->idx, preCnt);  //debug
	plk_push(net->queWaitRecycleSock, ctx->idx);
	++net->waitRecycleSockNum;	
}
static inline int close_ctx_fd(struct pps_net* net, struct socket_ctx* ctx)
{
	struct socket_sockctx* sock = &ctx->socks[0];
	sock_poll_del_fd(net->pollFd, sock);     // remove poll events
	sock_fd_close(&sock->fd);     //  close fd
	del_net_fd(net, &sock->fd);
	return 1;
}

static struct read_buf_block* expand_fill_buf(struct read_runtime* run, int recvBufLen)
{
	struct read_buf_block* buf = rdbuf_pop(&run->queFree);
	if (buf) {
		rdbuf_reset(buf, -1, ++run->fillBufIdx4Net);
	} else {
		buf = rdbuf_new(recvBufLen, ++run->fillBufIdx4Net);
	}
	rdbuf_push(&run->queReading, buf);
	run->curFillBuf = buf;
	run->fillBufIdx.store(run->fillBufIdx4Net, std::memory_order_release);
	return buf;
}
static int on_tcp_read(struct pps_net* net, struct socket_sockctx* sock)
{
	struct socket_ctx* ctx = sock->main; 
	struct read_runtime* run = &ctx->readRuntime;
	struct read_buf_block* buf;
	int32_t& waitReadable = run->waitReadable;
	if (!run->sockClosed) {
		// recycle bufs that has read
		uint32_t readBufIdx = run->readBufIdx.load(std::memory_order_acquire);
		while ((buf = rdbuf_front(&run->queReading))) {
			if (buf->idx == readBufIdx) {
				break;	
			}
			buf = rdbuf_pop(&run->queReading);
			rdbuf_push(&run->queFree, buf);
		}
		//
		buf = run->curFillBuf;
		if (buf->size4Fill >= buf->cap) {   // curFillBuf is full, prepare newBuf for fill
			buf = expand_fill_buf(run, ctx->recvBufLen);
		}
		int read,readable;
		while (true) {
			read = sock_tcp_read(sock->fd, buf->buf + buf->size4Fill, buf->cap - buf->size4Fill);
			if (read > 0) {   // read some
				buf->size4Fill += read;
				readable = run->readable.fetch_add(read, std::memory_order_release) + read;
				// check recvbuf is full
				if(readable >= ctx->recvBufLen && (waitReadable < 1|| readable >= waitReadable)){   // recvBuf full, pause pollIn
					ctx->pollInReg = false;
					poll_evt_mod(net->pollFd, sock);
					printf("on_tcp_read(), readBuf is full, remove pollIn\n");         // debug
					break;
				}
				if (buf->size4Fill >= buf->cap) {  // cur buf is full, expand
					buf = expand_fill_buf(run, ctx->recvBufLen);
				}
			} else {  //  no more data or conn has gone
				if(read != READ_RET_AGAIN) {  // close or halfClose
					run->sockClosed = true;
					sock_read_atom_release(ctx);        // make sockClosed visible for svc read
					if(read == READ_RET_HALF_CLOSE && ctx->pollOutReg) {  // half close & has unsend data 
						if(ctx->pollInReg) { // unReg pollIn
							ctx->pollInReg = false;
							poll_evt_mod(net->pollFd, sock);
						}
					}else {
						printf("on_tcp_read(), conn close\n");          // debug
						ctx->pollInReg = false;
						ctx->pollOutReg = false;
						sock_poll_del_fd(net->pollFd, sock);         // remove events reg
						//sock_fd_close(&sock->fd);         //  close fd	
						ctx->sendRuntime.sockClosed = true;
					}
				}
				break;
			}
		}
	}
	if (waitReadable > 0) {   // there is a readWait, notify
		if(run->sockClosed || run->readable.load(std::memory_order_relaxed) >= waitReadable) {
			// notify svc
			waitReadable = -1;
			struct tcp_read_wait_ret ret;
			ret.session = ctx->src.session;
			ret.sockId.idx = ctx->idx;
			ret.sockId.cnt = ctx->cnt4Net;
			send_readnotify_to_svc(net, &ctx->src, &ret);
		}
	}
	return 1;
}
static int on_tcp_send(struct pps_net* net, struct socket_sockctx* sock)
{
	struct socket_ctx* ctx = sock->main; 
	struct send_runtime* run = &ctx->sendRuntime;
	struct send_buf_block* buf;
	SOCK_FD* pFd = &ctx->socks[0].fd;
	int unSendOri = run->unSendBytes.load(std::memory_order_acquire);
	if (unSendOri < 1) {// no unsend data
		if(ctx->pollOutReg) {   // unreg pollOut
			ctx->pollOutReg = false;
			poll_evt_mod(net->pollFd, &ctx->socks[0]);
		}
	} else {
		int unSend = unSendOri;
		buf = run->curSendBuf;
		int sendRet;
		int sendLen;
		while (unSend > 0) {
			int bufLeft = buf->cap - buf->size4Send;
			if (bufLeft < 1) {   // curbuf has send all, move to next
				buf = buf->next;
				run->curSendBuf = buf;
				buf->size4Send = 0;
				bufLeft = buf->cap;
				run->sendBufIdx.fetch_add(1, std::memory_order_release);
			}
			if (unSend <= bufLeft) {
				sendLen = unSend;
			} else {
				sendLen = bufLeft;
			}
			sendRet = sock_tcp_send(*pFd, buf->buf + buf->size4Send, sendLen);
			if (sendRet > 0) {
				buf->size4Send += sendRet;
				unSend -= sendRet;
			} else {     // sock has gone or sendbuf is full
				if(sendRet == SEND_RET_CLOSE) {   // sendClosed
					run->sockClosed = true;
					if (ctx->pollOutReg) {    // unreg pollOut
						ctx->pollOutReg = false;
						poll_evt_mod(net->pollFd, &ctx->socks[0]);
					}
				}
				break;
			}
		}
		sendLen = unSendOri - unSend;
		if (sendLen > 0) {   // has send some
			run->unSendBytes.fetch_sub(sendLen, std::memory_order_release);
		}
	}
	if (!ctx->pollOutReg) {   // send task done, check close
		if(ctx->readOver4Net) {  // do close and recycle
			struct socket_sockctx* sock = &ctx->socks[0];
			if(!ctx->readRuntime.sockClosed || !ctx->sendRuntime.sockClosed) {  // do close
				ctx->pollInReg = false;
				ctx->pollOutReg = false;
				ctx->readRuntime.sockClosed = true;
				ctx->sendRuntime.sockClosed = true;
				sock_poll_del_fd(net->pollFd, sock);          // unReg events  
			}
			//
			if (!sock_send_trylock(ctx)) {
				while (!sock_send_trylock(ctx)) {}
			}
			ctx->sendRuntime.sockCnt = ctx->cnt4Net + 1;  // stop follow send()
			sock_send_unlock(ctx);
			//
			sock_fd_close(&sock->fd); 
			del_net_fd(net, &sock->fd);
			recycle_ctx(net, ctx);
		}
	}
	return 1;	
}
static int on_tcp_accept(struct pps_net* net, struct socket_sockctx* sock, SOCK_FD fd, SOCK_ADDR* addrRemote, int portRemote);
static bool do_tcp_listen(struct netreq_src* src, struct tcp_server_cfg* cfg, struct pps_net* net);
static bool do_tcp_conn(struct netreq_src* src, struct tcp_conn_info* cfg, struct pps_net* net);
static bool do_tcp_add(struct netreq_src* src, struct pps_net* net, struct tcp_add_info* info);
static bool do_tcp_close(struct netreq_src* src, struct pps_net* net);
static bool do_read_wait(struct netreq_src* src, struct tcp_read_wait* req, struct pps_net* net);
static bool do_read_over(struct netreq_src* src, struct tcp_read_wait* req, struct pps_net* net);
static bool do_send_wait(struct netreq_src* src, struct pps_net* net);

void net_thread(struct pps_net* net)
{
	printf("net(%d) start\n", net->index);  // debug
	struct pipes* pipes = net->pipes;
	uint32_t totalLoopNum = pipes->totalLoopThread.load();
	// add eventfd to poll
	sock_ctxsocks_init(&net->sockCtxEvent, 1);
	sock_ctxsocks_set(&net->sockCtxEvent, 0, net->eventFd, SOCKCTX_TYPE_EVENT);
	net->sockCtxEvent.pollInReg = true;
	net->sockCtxEvent.pollOutReg = false;
	sock_poll_add_fd(net->pollFd, &net->sockCtxEvent.socks[0], SOCK_POLLEVT_IN);
	// incr loop started num
	pipes->loopStartedNum.fetch_add(1);
	// wait all loop started
	while(pipes->loopStartedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("net(%d) will loop\n", net->index);
	//
	int ret, cnt;
	struct mq_mpsc<struct net_task_req>* queTaskReq = net->queTaskReq;
	struct pool_linked<struct net_msg_ori>* queUnSendMsg = net->queUnSendMsg;
	uint32_t& unSendMsgNum = net->unSendMsgNum;
	struct pool_linked<int32_t>* queWaitRecycle = net->queWaitRecycleSock;
	uint32_t& waitRecycleNum = net->waitRecycleSockNum;
	struct socket_mgr* pSockMgr = net->sockMgr;
	struct net_msg_ori* pMsg;
	struct net_task_req req;
	int32_t& pollWait = net->pollWait;
	struct minheap_queue<struct conn_wait_info*>* queConnTimeout = net->queConnTimeout;
	struct conn_wait_info* pConnWait;
	int32_t& connTimeoutNum = net->connTimeoutNum;
	bool shutdown = false;
	while(true) {
		// check task event
		while(mpsc_pop_custom(queTaskReq, &req, fn_pop_net_req)) { // has task req
			switch(req.cmd)
			{
			case NETCMD_READ_WAIT:
				do_read_wait(&req.src, (struct tcp_read_wait*)req.buf, net);
				break;
			case NETCMD_TCP_ADD:
				do_tcp_add(&req.src, net, (struct tcp_add_info*)req.buf);
				break;
			case NETCMD_READ_OVER:
				do_read_over(&req.src, (struct tcp_read_wait*)req.buf, net);
				break;
			case NETCMD_TCP_CLOSE:
				do_tcp_close(&req.src, net);
				break;
			case NETCMD_SEND_WAIT:
				do_send_wait(&req.src, net);
				break;
			case NETCMD_TCP_CONNECT:
				do_tcp_conn(&req.src, (struct tcp_conn_info*)req.buf, net);
				break;
			case NETCMD_TCP_LISTEN:   // do tcp listen
				do_tcp_listen(&req.src, (struct tcp_server_cfg*)req.buf, net);
				break;
			case NETCMD_SHUTDOWN:
				shutdown = true;
				break;
			default:   // unknown type, warning
				break;
			}
		}
		if(shutdown){
			break;
		}
		if (unSendMsgNum > 0) {   // has unsendmsg
			cnt = 0;
			const plk_iterator<struct net_msg_ori> it = plk_it_init(queUnSendMsg);
			for (; plk_it_loop(it);) {
				pMsg = *it;
				ret = send_to_svc(pMsg, net, false);
				if (ret != 0) {   //  send succ or svc has gone, remove
					plk_erase(it);
					--unSendMsgNum;
					if (++cnt >= 1000) {
						break;
					}
				}
			}
		}
		if (waitRecycleNum > 0) {   // has waitRecycle socks
			cnt = 0;
			const plk_iterator<int32_t> it = plk_it_init(queWaitRecycle);
			for (; plk_it_loop(it);) {
				int32_t sockIdx = **it;
				if (sock_slot_free(pSockMgr, sockIdx)) {
					plk_erase(it);
					--waitRecycleNum;
				}
				else {   // impossible
				
				}
				if (++cnt >= 100) {
					break;
				}
			}
		}
		if (unSendMsgNum > 0 || waitRecycleNum > 0) {
			ret = sock_poll(net, 0);
		}
		else {
			ret = sock_poll(net, pollWait);
		}
		// check connTimeout task
		if(connTimeoutNum > 0){   // has connTimeout task, check
			int64_t tmNow = timer_clock_now_ms(pipes);
			bool expired, waitTaskDone, waitTaskSucc;
			while( (pConnWait=minheap_get(queConnTimeout)) ){
				waitTaskDone = pConnWait->waitTaskDone;
				int off = pConnWait->expiration - tmNow;
				expired = off <= 0;
				if(expired || waitTaskDone){   // expire or connHasDone£¬ remove
					minheap_pop(queConnTimeout); // remove from queue
					--connTimeoutNum;
					waitTaskSucc = pConnWait->waitTaskSucc;
					printf("rmTimeout, expr=%d, connDone=%d, connSucc=%d\n", 
						expired, waitTaskDone, waitTaskSucc);   // debug
					bool doClose = false, doRecycle = false;
					if(waitTaskDone && !waitTaskSucc){   // waitTaskFailed, release and recycle
						doRecycle = true;
					}else if(expired && !waitTaskDone){   // expired and connNotDone
						doClose = true;
						doRecycle = true;
						// notify src
						struct tcp_conn_ret connRet;
						connRet.ret = SOCK_ERR_TIMEOUT;
						wrap_svc_msg(net->pNetMsgOri, &pConnWait->ctx->src, NETCMD_TCP_CONNECT, &connRet, sizeof(struct tcp_conn_ret));
						send_to_svc(net->pNetMsgOri, net, true);
					}
					if(doClose){
						struct socket_ctx* ctx = pConnWait->ctx;
						close_ctx_fd(net, ctx);
					}
					if(doRecycle){
						struct socket_ctx* ctx = pConnWait->ctx;
						ctx->ud = nullptr;
						delete pConnWait;
						recycle_ctx(net, ctx);   // recycle ctx
					}
					// fetch next
					pConnWait = minheap_get(queConnTimeout);
				}else {
					// update pollWait
					if(pollWait < 0 || off < pollWait){
						pollWait = off;
					}
					break;
				}
			}
			if(connTimeoutNum == 0){  // no connTimeout task, reset pollWait
				pollWait = -1;
			}
		}
	}
	// wait all extthread done
	pps_wait_ext_thread_done(pipes);
	//
	exclusive_mgr_waitalldone(pipes->exclusiveMgr);
	// decr loopExited num
	pipes->loopExitedNum.fetch_add(1, std::memory_order_relaxed);
	// wait all loop exit
	while(pipes->loopExitedNum.load(std::memory_order_relaxed) < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	// destroy res 
	// close all fd
	std::unordered_set<SOCK_FD>* pSetFd = net->pSetFd;
	std::unordered_set<SOCK_FD>::iterator it = pSetFd->begin();
	while(it != pSetFd->end()){
		SOCK_FD fd = *it;
		sock_fd_close(&fd);
		++it;
	}
	while(mpsc_pop_custom(queTaskReq, &req, fn_pop_net_req)) {
		int cmd = req.cmd;
		if(cmd == NETCMD_TCP_ADD){
			struct socket_ctx* ctx = sock_get_ctx(net->sockMgr, req.src.idx);
			sock_fd_close(&ctx->socks[0].fd);
		}else if(cmd == NETCMD_TCP_CONNECT){
			struct tcp_conn_info* info = (struct tcp_conn_info*)req.buf;
			sock_fd_close(&info->fd);
		}
	}
	printf("net(%d) end\n", net->index);  // debug
}

static inline struct socket_ctx* get_valid_sock(struct socket_mgr* mgr, int idx, uint32_t cnt);
static bool do_tcp_add(struct netreq_src* src, struct pps_net* net, struct tcp_add_info* info)
{
	struct socket_ctx* ctx = sock_get_ctx(net->sockMgr, src->idx);
	add_net_fd(net, &ctx->socks[0].fd);
	SOCK_RT_BEGIN(ctx);
	sock_poll_init_tcpfd(&ctx->socks[0], ctx->sendBufLen, ctx->recvBufLen);
	bool bo = ctx->closeCalled4Net;
	printf("do_tcp_add(), fd=%d\n", ctx->socks[0].fd);   // debug
	bo = ctx->readOver4Net;
	ctx->pollInReg = true;
	ctx->pollOutReg = false;
	poll_evt_add(net->pollFd, &ctx->socks[0]);
	SOCK_RT_END(ctx);
	// ensure read-runtime visible begin
	sock_read_atom_acquire(ctx);
	struct read_runtime* run = &ctx->readRuntime;
	bo = run->sockClosed;
	struct read_buf_block* b = run->curFillBuf;
	int32_t n = b->size4Fill;
	n = b->cap;
	n = run->waitReadable;
	uint32_t un = run->fillBufIdx4Net;
	struct read_buf_queue* q = &run->queReading;
	q = &run->queFree;
	// ensure read-runtime visible end
	// ensure send-runtime visible
	struct send_runtime* srun = &ctx->sendRuntime;
	srun->sendLock.load(std::memory_order_acquire);
	bo = srun->sockClosed;
	struct send_buf_block* sbuf = srun->curSendBuf;
	sbuf->size4Send = 0;
	// notify src
	struct tcp_conn_in m;
	m.sockId.idx = ctx->idx;
	m.sockId.cnt = ctx->cnt4Net;
	m.sockIdParent.idx = info->parentIdx;
	m.sockIdParent.cnt = info->parentCnt;
	wrap_svc_msg(net->pNetMsgOri, &info->src, NETCMD_TCP_CONNIN, &m, sizeof(m));
	int sendRet = send_to_svc(net->pNetMsgOri, net, true);
	if (sendRet < 0) {   // dst svc has gone
		close_ctx_fd(net, ctx);
		// recycle ctx
		recycle_ctx(net, ctx);
		return false;
	}
	return true;
}
static void send_runtime_init(struct socket_ctx* ctx, int sendBufLen, uint32_t sockCnt);
static bool do_tcp_listen(struct netreq_src* src, struct tcp_server_cfg* cfg, struct pps_net* net)
{
	int err = SOCK_ERR_UNKNOWN;
	struct socket_ctx* ctx = nullptr;
	do {
		struct socket_mgr* sockMgr = net->sockMgr;
		int32_t idxSlot = sock_slot_alloc(sockMgr);
		if (idxSlot < 0) {   // slot is full
			err = SOCK_ERR_CTX_FULL;
			break;
		}
		ctx = sock_get_ctx(sockMgr, idxSlot);
		SOCK_RT_BEGIN(ctx);
		sock_ctxsocks_init(ctx, cfg->addrNum);
		for (int i=0; i<ctx->sockCap; ++i) {
			sock_fd_invalid(&ctx->socks[i].fd);
		}
		int errIdx;
		if (!sock_tcp_listen(cfg, ctx->socks, ctx->sockCap, &err, &errIdx)) { // do listen failed
			ctx->ud = nullptr;
			SOCK_RT_END(ctx);
			sock_slot_free(sockMgr, idxSlot);    // free slot
			break;
		}
		struct tcp_server_cfg* cfgCp = new struct tcp_server_cfg;
		*cfgCp = *cfg;
		ctx->ud = cfgCp;
		ctx->type = SOCKCTX_TYPE_TCP_LISTEN;
		ctx->pollInReg = true;
		ctx->pollOutReg = false;
		for (int i=0; i<cfg->addrNum; ++i) {
			struct socket_sockctx* sock = &ctx->socks[i];
			sock->type = SOCKCTX_TYPE_TCP_LISTEN;
			sock_poll_add_fd(net->pollFd, sock, SOCK_POLLEVT_IN);
			add_net_fd(net, &sock->fd);
		}
		ctx->sockNum = cfg->addrNum;
		ctx->idxNet = net->index;
		ctx->src = *src;
		ctx->cnt4Net = ctx->cnt.load(std::memory_order_relaxed);
		ctx->closeCalled4Net = false;
		SOCK_RT_END(ctx);
		//
		send_runtime_init(ctx, cfgCp->sendBuf, ctx->cnt4Net);
		//
		err = 0;
	} while (false);
	struct tcp_listen_ret ret;
	ret.ret = err;
	//
	if (err == 0) { // succ
	    printf("listen at %d succ, backlog=%d, snd=%d, rcv=%d\n",  
			cfg->port, cfg->backlog, cfg->sendBuf, cfg->recvBuf);     // debug
		ret.sockId.idx = ctx->idx;
		ret.sockId.cnt = ctx->cnt4Net;
	}
	else {
		protocol_cfg_free(cfg->protocolCfg);
		printf("listen at %d failed, err=%d, backlog=%d, snd=%d, rcv=%d\n",  
			cfg->port, err, cfg->backlog, cfg->sendBuf, cfg->recvBuf);     // debug
		ret.sockId.idx = -1;
	}
	// send to svc
	wrap_svc_msg(net->pNetMsgOri, src, NETCMD_TCP_LISTEN, &ret, sizeof(ret));
	send_to_svc(net->pNetMsgOri, net, true);
	return true;
}
static bool do_read_wait(struct netreq_src* src, struct tcp_read_wait* req, struct pps_net* net)
{
	struct socket_ctx* ctx = sock_get_ctx(net->sockMgr, req->sockId.idx);
	struct read_runtime* run = &ctx->readRuntime;
	assert(run->waitReadable == -1);   // debug
	if (req->waitReadable <= run->readable.load(std::memory_order_relaxed) 
			|| run->sockClosed
			|| ctx->closeCalled4Net) {
		//
		struct tcp_read_wait_ret ret;
		ret.session = src->session;
		ret.sockId.idx = req->sockId.idx;
		ret.sockId.cnt = req->sockId.cnt;
		send_readnotify_to_svc(net, src, &ret);
		return true;
	}
	run->waitReadable = req->waitReadable;
	ctx->src = *src;
	if (!ctx->pollInReg) {   // enable pollIn
		ctx->pollInReg = true;
		poll_evt_mod(net->pollFd, &ctx->socks[0]);
		printf("doReadWait(), reg pollIn\n");  // debug
	}
	return true;
}
static bool do_read_over(struct netreq_src* src, struct tcp_read_wait* req, struct pps_net* net)
{
	struct socket_ctx* ctx = sock_get_ctx(net->sockMgr, req->sockId.idx);
	assert(!ctx->readOver4Net);    // debug
	//
	printf("do_read_over(), pollOutReg=%d\n", ctx->pollOutReg);  // debug
	ctx->readOver4Net = true;
	if (ctx->pollOutReg) {   // has sendTask
		return false;
	}
	// no send task, do close(if need) and recycle
	struct socket_sockctx* sock = &ctx->socks[0];
	if (!ctx->readRuntime.sockClosed) {   // read-in not closed
		ctx->pollInReg = false;
		ctx->readRuntime.sockClosed = true;
		ctx->sendRuntime.sockClosed = true;
		sock_poll_del_fd(net->pollFd, sock);         // unReg events    
	}
	// do recycle
	if (!sock_send_trylock(ctx)) {
		while (!sock_send_trylock(ctx)) {}
	}
	ctx->sendRuntime.sockCnt = ctx->cnt4Net + 1;  // stop follow send()
	sock_send_unlock(ctx);
	//
	if(!ctx->closeCalled4Net){
		sock_fd_close(&sock->fd);
		del_net_fd(net, &sock->fd);
	}
	recycle_ctx(net, ctx);
	return true;
}
static bool do_send_wait(struct netreq_src* src, struct pps_net* net)
{
	struct socket_ctx* ctx = get_valid_sock(net->sockMgr, src->idx, src->cnt);
	if (ctx == nullptr) {  // sockCtx has gone
		return false;
	}
	if (ctx->closeCalled4Net) {  // closeCalled
		return false;
	}
	struct send_runtime* run = &ctx->sendRuntime;
	if (run->sockClosed) {
		return false;
	}
	if (!ctx->pollOutReg) {  // reg pollOut
		ctx->pollOutReg = true;
		poll_evt_mod(net->pollFd, &ctx->socks[0]);
	}
	return true;
}
static bool do_tcp_close(struct netreq_src* src, struct pps_net* net)
{
	struct socket_ctx* ctx = get_valid_sock(net->sockMgr, src->idx, src->cnt);
	if (ctx == nullptr) {  // sockCtx has gone(closed), do nothing
		return false;
	}
	SOCK_RT_BEGIN(ctx);
	//printf("do_tcp_close() called, idx=%d, cnt=%d\n", src->idx, src->cnt);  // debug
	if (ctx->closeCalled4Net) {	// duplicate called!! impossible
		printf("WARNING!!! do_tcp_close() duplicate called! called=%d, idx=%d, cnt=%d\n",
			ctx->closeCalled4Net, src->idx, src->cnt);    // debug
		return false;
	}
	ctx->closeCalled4Net = true;
	if (ctx->type == SOCKCTX_TYPE_TCP_CHANNEL) {
		struct read_runtime* rrun = &ctx->readRuntime;
		//struct send_runtime* srun = &ctx->sendRuntime;
		if (ctx->readOver4Net) {  // has recv read-over, do nothing
			return false;
		}
		if (ctx->pollOutReg) {   // has sendWait task, do nothing
			if(ctx->pollInReg && !rrun->sockClosed) {   // remove pollIn event(stop reading more)
				ctx->pollInReg = false;
				poll_evt_mod(net->pollFd, &ctx->socks[0]);
				printf("do_tcp_close(), remove pollIn\n");           // debug
			}
			return false;
		}
		// close fd
		SOCK_FD* pFd = &ctx->socks[0].fd;
		rrun->sockClosed = true;
		sock_read_atom_release(ctx);        // make sockClosed visible for svc read
		ctx->pollInReg = false;
		ctx->pollOutReg = false;
		sock_poll_del_fd(net->pollFd, &ctx->socks[0]);         // remove events reg
		ctx->sendRuntime.sockClosed = true;
		sock_fd_close(pFd);
		del_net_fd(net, pFd);
		if (rrun->waitReadable > 0) {   // there is a readWait, notify src
			rrun->waitReadable = -1;
			struct tcp_read_wait_ret ret;
			ret.session = ctx->src.session;
			ret.sockId.idx = ctx->idx;
			ret.sockId.cnt = ctx->cnt4Net;
			send_readnotify_to_svc(net, &ctx->src, &ret);
		}
	} else if (ctx->type == SOCKCTX_TYPE_TCP_LISTEN) {
		for (int i = 0; i < ctx->sockNum; ++i) {
			struct socket_sockctx* sock = &ctx->socks[i];
			close_ctx_fd(net, ctx);
			sock_fd_invalid(&sock->fd);
		}
		struct tcp_server_cfg* cfg = (struct tcp_server_cfg*)ctx->ud;
		protocol_cfg_free(cfg->protocolCfg);
		cfg->protocolCfg = nullptr;
		delete cfg;
		ctx->ud = nullptr;
		// recycle
		recycle_ctx(net, ctx);
	}
	SOCK_RT_END(ctx);
	return true;
}

static int send_to_svc(struct net_msg_ori* msg, struct pps_net* net, bool addToWait) {
	if (msg->to.session > 0) {
		msg->to.session = -msg->to.session;
	}
	int ret = svc_send_netmsg(msg->to.idx, msg, net->pipes);
	if (ret > 0) {  //send succ
		return 1;
	}
	if (ret == 0) {   // svc mq is full
		if(addToWait) {
			plk_push_ptr(net->queUnSendMsg, msg);
			++net->unSendMsgNum;
		}
		return 0;
	}
	// svc has gone 
	return ret;
}
static int send_to_svc_ext(struct net_msg_ori* msg, struct pipes* pipes) {
	if (msg->to.session > 0) {
		msg->to.session = -msg->to.session;
	}
	int ret = svc_send_netmsg(msg->to.idx, msg, pipes);
	if (ret > 0) {  //send succ
		return 1;
	}
	if (ret == 0) {   // svc mq is full
		while((ret = svc_send_netmsg(msg->to.idx, msg, pipes)) == 0) {
		
		}
	} 
	return ret;
}

//
static void send_runtime_init(struct socket_ctx* ctx, int sendBufLen, uint32_t sockCnt) 
{
	assert(sock_send_trylock(ctx));
	struct send_runtime* run = &ctx->sendRuntime;
	run->unSendBytes.load(std::memory_order_acquire);
	struct send_buf_block* buf;
	while ((buf = sdbuf_pop(&run->queUnSend)) != nullptr) {
		sdbuf_push(&run->queFree, buf);
	}
	// init 1st buf
	buf = sdbuf_pop(&run->queFree);
	if (buf) {
		sdbuf_reset(buf, sendBufLen, 0);
	}
	else {
		buf = sdbuf_new(sendBufLen, 0);
	}
	sdbuf_push(&run->queUnSend, buf);
	run->curFillBuf = buf;
	run->curSendBuf = buf;
	run->fillBufIdx.store(0, std::memory_order_relaxed);
	run->sendBufIdx.store(0, std::memory_order_relaxed);
	run->fillBufIdxLocal = 0;
	//
	run->unSendBytes.store(0, std::memory_order_release);
	run->closeCalled4Svc = false;
	run->preLockOwner = 0;
	run->sockCnt = sockCnt;
	run->sockClosed = false;
	sock_send_unlock(ctx);
}
static void read_runtime_init(struct socket_ctx* ctx, int recvBufLen, uint32_t sockCnt) 
{
	struct read_runtime* run = &ctx->readRuntime;
	assert(sock_read_trylock(ctx));
	run->onReadWait = false;
	struct read_buf_block* buf;
	while ( (buf = rdbuf_pop(&run->queReading)) != nullptr ) {
		rdbuf_push(&run->queFree, buf);
	}
	// init 1st buf
	buf = rdbuf_pop(&run->queFree);
	if (buf) {
		rdbuf_reset(buf, recvBufLen, 0);
	}
	else {
		buf = rdbuf_new(recvBufLen, 0);
	}
	rdbuf_push(&run->queReading, buf);
	run->curFillBuf = buf;
	run->curReadBuf = buf;
	run->waitReadable = -1;
	run->fillBufIdx4Net = 0;
	run->fillBufIdx.store(0, std::memory_order_relaxed);
	run->readBufIdx.store(0, std::memory_order_relaxed);
	run->readable.store(0, std::memory_order_relaxed);
	run->preLockOwner = 0;
	run->sockCnt = sockCnt;
	run->sockClosed = false;
	run->hasSendReadOver = false;
	sock_read_unlock(ctx);
}

static void init_tcp_channel_ctx(struct pps_net* net, struct socket_ctx* ctx, 
	int sendBuf, int recvBuf, SOCK_FD* pFd, SOCK_ADDR* pAddrRemote, int portRemote)
{
	ctx->idxNet = net->index;
	ctx->type = SOCKCTX_TYPE_TCP_CHANNEL;
	sock_ctxsocks_init(ctx, 1);
	sock_ctxsocks_set(ctx, 0, pFd, SOCKCTX_TYPE_TCP_CHANNEL);
	ctx->sockNum = 1;
	ctx->sendBufLen = sendBuf;
	ctx->recvBufLen = recvBuf;
	ctx->addrRemote = *pAddrRemote;
	ctx->portRemote = portRemote;
	ctx->closeCalled4Net = false;
	ctx->readOver4Net = false;
	ctx->cnt4Net = ctx->cnt.load(std::memory_order_relaxed);
	send_runtime_init(ctx, ctx->sendBufLen, ctx->cnt4Net);   // do not swap down(ensure closeCalled visible for read())
	read_runtime_init(ctx, ctx->recvBufLen, ctx->cnt4Net);   // do not swap up(ensure closeCalled visible for read())
}
static inline void enreq_tcp_add(struct net_task_req* t, struct netreq_tcp_add* req);
static int on_tcp_accept(struct pps_net* net, struct socket_sockctx* sock, SOCK_FD fd, SOCK_ADDR* addrRemote, int portRemote)
{
	struct socket_ctx* mainCtx = sock->main;
	struct tcp_server_cfg* svrCfg = (struct tcp_server_cfg*)mainCtx->ud;
	/*
	// debug begin
	sock_addr_ntop(addrRemote, net->bufAddr, SOCK_ADDR_STRLEN);
	printf("on_tcp_accept, newConnIn: %s:%d\n", net->bufAddr, portRemote);
	sock_addr_ntop(&svrCfg->addrs[sock->idx], net->bufAddr, SOCK_ADDR_STRLEN);
	printf("newConn accept by %s:%d\n", net->bufAddr, svrCfg->port);
	// debug end
	*/
	int32_t idxCtx = sock_slot_alloc(net->sockMgr);
	if (idxCtx < 0) {   // no more free slot
		printf("on_tcp_accept(), no more sockCtxSlot to alloc\n");   // debug
		return 0;
	}
	struct pps_net* netDst = alloc_net(net->pipes);
	struct socket_ctx* ctxDst = sock_get_ctx(net->sockMgr, idxCtx);
	SOCK_RT_BEGIN(ctxDst);
	init_tcp_channel_ctx(netDst, ctxDst, svrCfg->sendBuf, svrCfg->recvBuf, 
		&fd, addrRemote, portRemote);
	SOCK_RT_END(ctxDst);
	printf("on_tcp_accept(), idx=%d, cnt=%d, fd=%d\n", ctxDst->idx, ctxDst->cnt4Net, fd);  // debug
	// send to net
	struct tcp_add_info info;
	info.src = mainCtx->src;
	info.parentIdx = mainCtx->idx;
	info.parentCnt = mainCtx->cnt4Net;
	struct netreq_tcp_add req;
	req.sockId.idx = ctxDst->idx;
	req.sockId.cnt = ctxDst->cnt4Net;
	req.info = &info;
	send_to_net(netDst, &req, enreq_tcp_add, net);
	return 1;
}

static int on_tcp_conn_done(struct pps_net* net, struct socket_sockctx* sock, int errCode)
{
	struct socket_ctx* ctx = sock->main;
	struct conn_wait_info* waitInfo = (struct conn_wait_info*)ctx->ud;
	bool closeFd = false;
	bool recycleCtx = false;
	bool inTimedoutQueue = waitInfo->inTimedoutQueue;
	bool hasTimedout = waitInfo->hasTimedout;
	printf("on_tcp_conn_done, checkConnRet=%d,inTmQ=%d, hasTmOut=%d\n", 
		errCode,inTimedoutQueue,hasTimedout);  // debug
	//
	if(inTimedoutQueue && hasTimedout){   // has timedout, close & release
		closeFd = true;
		recycleCtx = true;
	}else { 
		waitInfo->waitTaskDone = 1;
		struct tcp_conn_ret m;
		if(errCode == 0){  // conn succ
			waitInfo->waitTaskSucc = 1;
			SOCK_RT_BEGIN(ctx);
			init_tcp_channel_ctx(net, ctx, ctx->sendBufLen, ctx->recvBufLen, &sock->fd, &ctx->addrRemote, ctx->portRemote);
			ctx->pollInReg = true;
			ctx->pollOutReg = false;
			poll_evt_mod(net->pollFd, &ctx->socks[0]);   // reg event
			SOCK_RT_END(ctx);
			// 
			m.ret = 0;
			m.sockId.idx = ctx->idx;
			m.sockId.cnt = ctx->cnt4Net;
		}else {  // conn failed
			closeFd = true;
			if(!inTimedoutQueue){   // not in timeoutQueue, release
				recycleCtx = true;
			}
			//
			m.ret = errCode;
		}
		// notify src
		wrap_svc_msg(net->pNetMsgOri, &ctx->src, NETCMD_TCP_CONNECT, &m, sizeof(m));
		int sendRet = send_to_svc(net->pNetMsgOri, net, true);
		if (sendRet < 0) {   // dst svc has gone
			closeFd = true;
			if(!inTimedoutQueue){   // not in timeoutQueue, release
				recycleCtx = true;
			}
		}
	}
	if(closeFd){
		waitInfo->waitTaskSucc = 0;
		close_ctx_fd(net, ctx);
	}
	if(recycleCtx){
		ctx->ud = nullptr;
		delete waitInfo;
		recycle_ctx(net, ctx);
	}
	return 1;	
}
static bool do_tcp_conn(struct netreq_src* src, struct tcp_conn_info* info, struct pps_net* net)
{
	add_net_fd(net, &info->fd);
	struct socket_ctx* ctx = nullptr;
	struct tcp_conn_ret m;
	bool notifySrc = false;
	int32_t idxCtx = sock_slot_alloc(net->sockMgr);
	if (idxCtx < 0) {   // no more free slot
		sock_fd_close(&info->fd);
		del_net_fd(net, &info->fd);
		m.ret = SOCK_ERR_CTX_FULL;
		notifySrc = true;
	}else{
		ctx = sock_get_ctx(net->sockMgr, idxCtx);
		if(info->inProgress){  // in progress, reg pollOut
			SOCK_RT_BEGIN(ctx);
			sock_ctxsocks_init(ctx, 1);
			sock_ctxsocks_set(ctx, 0, &info->fd, SOCKCTX_TYPE_TCP_CONN_WAIT);
			ctx->sockNum = 1;
			ctx->sendBufLen = info->sendbuf;
			ctx->recvBufLen = info->recvbuf;
			ctx->portRemote = info->portRemote;
			ctx->addrRemote = info->addrRemote;
			ctx->src = *src;
			SOCK_RT_END(ctx);
			struct conn_wait_info* waitInfo = new struct conn_wait_info;
			waitInfo->waitTaskDone = 0;
			waitInfo->waitTaskSucc = 0;
			ctx->ud = waitInfo;
			waitInfo->ctx = ctx;
			//
			struct socket_sockctx* sock = &ctx->socks[0];
			ctx->pollInReg = false;
			ctx->pollOutReg = true;
			poll_evt_add(net->pollFd, sock);
			//
			if(info->timeout > 0){  // add timeout task
				minheap_add(waitInfo, net->queConnTimeout);
				++net->connTimeoutNum;
				//
				waitInfo->timeout = info->timeout;
				waitInfo->expiration = info->expiration;
				waitInfo->hasTimedout = 0;
				waitInfo->inTimedoutQueue = 1;
				//
				int64_t tmNow = timer_clock_now_ms(net->pipes);
				int delay = waitInfo->expiration - tmNow;
				if(delay < 0){
					delay = 0;
				}
				if(net->pollWait < 0 || delay < net->pollWait){  // update pollWait timeout
					net->pollWait = delay;
				}
			}else {   // conn wait indefinitely
				waitInfo->inTimedoutQueue = 0;
			}
		}else{   // conn succ
			SOCK_RT_BEGIN(ctx);
			init_tcp_channel_ctx(net, ctx, info->sendbuf, info->recvbuf, &info->fd, &info->addrRemote, info->portRemote);
			ctx->pollInReg = true;
			ctx->pollOutReg = false;
			poll_evt_add(net->pollFd, &ctx->socks[0]);   // reg event
			SOCK_RT_END(ctx);
			// 
			m.ret = 0;
			m.sockId.idx = ctx->idx;
			m.sockId.cnt = ctx->cnt4Net;
			notifySrc = true;
		}
	}
	// notify src
	if(notifySrc){
		wrap_svc_msg(net->pNetMsgOri, src, NETCMD_TCP_CONNECT, &m, sizeof(m));
		int sendRet = send_to_svc(net->pNetMsgOri, net, true);
		if (sendRet < 0) {   // dst svc has gone
			if(m.ret == 0){   // init done
				//sock_poll_del_fd(&net->pollFd, &ctx->socks[0]);     // remove poll events
				//sock_fd_close(&ctx->socks[0].fd);     //  close fd
				close_ctx_fd(net, ctx);
			}
			// recycle ctx
			recycle_ctx(net, ctx);
			return false;
		}
	}
	return true;
}

static inline struct socket_ctx* get_sock_by_idx(struct socket_mgr* mgr, int idx)
{
	if (idx < 0 || idx >= SOCK_CTX_SLOT_NUM) {
		return nullptr;
	}
	return sock_get_ctx(mgr, idx);
}
static inline struct socket_ctx* get_valid_sock(struct socket_mgr* mgr, int idx, uint32_t cnt)
{
	struct socket_ctx* ctx = get_sock_by_idx(mgr, idx);
	if (ctx) {
		if (cnt != ctx->cnt.load(std::memory_order_acquire)) {
			return nullptr;
		}
	}
	return ctx;
}
int net_get_remote(struct pipes* pipes, int idx, uint32_t cnt, char* buf, int szBuf, int* port)
{
	struct socket_ctx* ctx = get_valid_sock(pipes->sockMgr, idx, cnt);
	if (ctx == nullptr) {
		return 0;
	}
	SOCK_RT_BEGIN(ctx);
	if (!sock_addr_ntop(&ctx->addrRemote, buf, szBuf)) {
		return 0;
	}
	*port = ctx->portRemote;
	return 1;
}

static inline void enreq_send_wait(struct net_task_req* t, struct netreq_send_wait* req);
int net_tcp_send(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt, const char* data, int size)
{
	struct socket_ctx* ctx = get_valid_sock(pipes->sockMgr, sockIdx, sockCnt);
	if (ctx == nullptr) {
		return 0;
	}
	struct send_runtime* run = &ctx->sendRuntime;
	if (!sock_send_trylock(ctx)) {    // concurrent
		while (!sock_send_trylock(ctx)) {
		}
		if (sockCnt != run->sockCnt) {  // sockCtx has released
			sock_send_unlock(ctx);
			return 0;
		}
		if (run->preLockOwner == 1) {   // concurrent with another send call, invalid
			sock_send_unlock(ctx);
			return -1;
		}
	}
	run->preLockOwner = 1;   // mark lock by send()
	if(run->closeCalled4Svc) {   // has called close
		sock_send_unlock(ctx);
		return 0;
	}
	struct send_buf_block* buf;
	int unSendBytes = run->unSendBytes.load(std::memory_order_acquire);
	int hasSend = 0;
	if (unSendBytes < 1) {   // can direct send
		hasSend = sock_tcp_send(ctx->socks[0].fd, data, size);
		if (hasSend >= size) {  // send all succ
			sock_send_unlock(ctx);
			return 1;
		}
		if (hasSend == SEND_RET_CLOSE) {  // send channel has closed
			sock_send_unlock(ctx);
			return 0;
		}
		// send parts
		if(hasSend == SEND_RET_AGAIN) {// send nothing, sendbuf is full
			hasSend = 0;
		}
	}
	// recycle buf
	int sendBufIdx = run->sendBufIdx.load(std::memory_order_relaxed);
	while ((buf = sdbuf_front(&run->queUnSend))) {
		if (buf->idx == sendBufIdx) {
			break;	
		}
		buf = sdbuf_pop(&run->queUnSend);
		sdbuf_push(&run->queFree, buf);
	}
	// add to queue
	int waitSendOri = size - hasSend;
	int waitSend = waitSendOri;
	buf = run->curFillBuf;
	while (waitSend > 0) {
		int curBufleft = buf->cap - buf->size4Fill;
		if (curBufleft < 1) {   // curFillBuf is full, alloc new
			buf = sdbuf_pop(&run->queFree);
			if (buf) {
				sdbuf_reset(buf, -1, ++run->fillBufIdxLocal);
			}
			else {
				buf = sdbuf_new(ctx->sendBufLen, ++run->fillBufIdxLocal);
			}
			sdbuf_push(&run->queUnSend, buf);
			run->curFillBuf = buf;
			curBufleft = buf->cap;
			run->fillBufIdx.store(run->fillBufIdxLocal, std::memory_order_release);
		}
		if (waitSend <= curBufleft) {
			memcpy(buf->buf + buf->size4Fill, data + hasSend, waitSend);
			buf->size4Fill += waitSend;
			break;
		}
		else {
			memcpy(buf->buf + buf->size4Fill, data + hasSend, curBufleft);
			buf->size4Fill = buf->cap;
			hasSend += curBufleft;
			waitSend -= curBufleft;
		}
	}
	unSendBytes = run->unSendBytes.fetch_add(waitSendOri, std::memory_order_release);
	if (unSendBytes < 1) {   // notify net
		struct netreq_send_wait req;
		req.src.idx = sockIdx;
		req.src.cnt = sockCnt;
		send_to_net(net_get(pipes, ctx->idxNet), &req, enreq_send_wait, nullptr);	
	}
	sock_send_unlock(ctx);
	return 1;
}
static inline void enreq_tcp_close(struct net_task_req* t, struct netreq_tcp_close* req);
int net_close_sock(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt)
{
	struct socket_ctx* ctx = get_valid_sock(pipes->sockMgr, sockIdx, sockCnt);
	if (ctx == nullptr) {
		return 0;
	}
	struct send_runtime* run = &ctx->sendRuntime;
	if (!sock_send_trylock(ctx)) {  // concurrent
		while(!sock_send_trylock(ctx)) {
		}
		if (sockCnt != run->sockCnt) {  // sockCtx has released
			sock_send_unlock(ctx);
			return 0;
		}
	}
	run->preLockOwner = 2;    // mark lock by close()
	if(run->closeCalled4Svc) {  //  close() has called
		sock_send_unlock(ctx);
		return 0;
	}
	run->closeCalled4Svc = true;
	//printf("sockCloseTrace: idx=%d, cnt=%d", sockIdx, sockCnt);  //debug
	sock_read_atom_release(ctx);  // make closeCalled=true visible for read()
	run->sockCnt = ctx->cnt4Net + 1;      // stop follow send() and close()
	sock_send_unlock(ctx);
	// notify net
	struct netreq_tcp_close req;
	req.sockId.idx = sockIdx;
	req.sockId.cnt = sockCnt;
	struct pps_net* net = net_get(pipes, ctx->idxNet);
	send_to_net(net, &req, enreq_tcp_close, nullptr);
	return 1;
}
int net_is_listen_valid(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt)
{
	struct socket_ctx* ctx = get_valid_sock(pipes->sockMgr, sockIdx, sockCnt);
	if (ctx == nullptr) {
		return 0;
	}
	sock_send_atom_acquire(ctx);
	struct send_runtime* run = &ctx->sendRuntime;
	if (sockCnt != run->sockCnt) {
		return 0;
	}
	if (run->closeCalled4Svc) {
		return 0;
	}
	return 1;
}

static inline void enreq_read_wait(struct net_task_req* t, struct netreq_read_wait* req);
static inline void enreq_read_over(struct net_task_req* t, struct netreq_read_wait* req);
static void send_read_over(struct pps_net* net, int32_t sockIdx, uint32_t sockCnt, struct read_arg* arg)
{
	struct tcp_read_wait wait;
	wait.sockId.idx = sockIdx;
	wait.sockId.cnt = sockCnt;
	struct netreq_read_wait req;
	req.src.idx = arg->srcIdx;
	req.src.cnt = arg->srcCnt;
	req.src.session = arg->session;
	req.wait = &wait;
	send_to_net(net, &req, enreq_read_over, nullptr);
}
int net_tcp_read(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt, struct read_arg* arg, int*trunc)
{
	struct socket_ctx* ctx = get_valid_sock(pipes->sockMgr, sockIdx, sockCnt);
	if (ctx == nullptr) {
		return -1;
	}
	struct read_runtime* run = &ctx->readRuntime;
	if (!sock_read_trylock(ctx)) {   //  concurrent
		while(!sock_read_trylock(ctx)) {
		}
		if (sockCnt != run->sockCnt) {   // sockCtx has gone
			sock_read_unlock(ctx);
			return -1;
		}
		if (run->preLockOwner == 1) {   // concurrent with another read(), invalid
			sock_read_unlock(ctx);
			return -2;
		}
	}
	if (ctx->sendRuntime.closeCalled4Svc) {   // close() called
		bool hasSendReadOver = run->hasSendReadOver;
		run->hasSendReadOver = true;
		sock_read_unlock(ctx);
		if(!hasSendReadOver){   // send read over
			send_read_over(net_get(pipes, ctx->idxNet), sockIdx, sockCnt, arg);
		}
		return -1;
	}
	run->preLockOwner = 1;   //  mark lock by read()
	int decType = -1;
	if(run->onReadWait) {    // onReadWait
		if(arg->isNewRead) {
			sock_read_unlock(ctx);
			return -3;
		}
		decType = run->curDec->type;
	}
	else {  // not onRead
		if(!arg->isNewRead) { //  arg invalid
			sock_read_unlock(ctx);
			return -4;
		}
		decType = arg->decType;
		s_arrDecInit[decType](run, arg);    // init decode
		run->onReadWait = true;
		run->cbRead = arg->cb;
		run->readPackMax = arg->maxRead;
	}
	run->udRead = arg->ud;
	int waitReadable = -1;
	int ret = s_arrDecRead[decType](run, &waitReadable, trunc);
	if (ret) {   // read done
		run->onReadWait = false;
	}
	else {   // readWait
		if (run->sockClosed) {   // read sock closed, do last read
			run->onReadWait = false;
			ret = s_arrDecRead[decType](run, &waitReadable, trunc);   // try again, the buf may recv more data after "readAbove"
			if(ret) {   //read succ
				sock_read_unlock(ctx);
				return 1;
			}
			if (decType != DECTYPE_NOW) {   // read all left
				ret = s_arrDecRead[DECTYPE_NOW](run, &waitReadable, trunc);
			}
			run->sockCnt = ctx->cnt4Net + 1;        // incr sockCnt to forbid follow read()
			// tell net read() over
			bool hasSendReadOver = run->hasSendReadOver;
			run->hasSendReadOver = true;
			sock_read_unlock(ctx);
			if(!hasSendReadOver){
				send_read_over(net_get(pipes, ctx->idxNet), sockIdx, sockCnt, arg);
			}
			if (ret) {   // read sth
				return 2;    // means last read
			} else {  // no data has read
				return -1;     // means sock closed
			}
		}
		else {    //  not read anything, send readWait req to net
			struct tcp_read_wait wait;
			wait.waitReadable = waitReadable;
			wait.sockId.idx = sockIdx;
			wait.sockId.cnt = sockCnt;
			struct netreq_read_wait req;
			req.src.idx = arg->srcIdx;
			req.src.cnt = arg->srcCnt;
			req.src.session = arg->session;
			req.wait = &wait;
			send_to_net(net_get(pipes, ctx->idxNet), &req, enreq_read_wait, nullptr);	
			sock_read_unlock(ctx);
			return 0;
		}
	}
	sock_read_unlock(ctx);
	return 1;	
}

static int read_now_inner(struct read_runtime* run, int readableOri, int* waitReadable, int* trunc)
{
	if (readableOri < 1) {   // no readable data
		*waitReadable = 1;
		return 0;
	}
	if(readableOri > run->readPackMax){   // need trunc
		readableOri = run->readPackMax;
		*trunc = 1;
	}else{
		*trunc = 0;
	}
	//uint32_t fillBufIdx = run->fillBufIdx.load(std::memory_order_acquire);
	struct read_buf_block* buf = run->curReadBuf;
	int writeCnt = 0;
	int readable = readableOri;
	while (readable > 0) {
		int bufLeft = buf->cap - buf->size4Read;
		if (bufLeft < 1) {  // curReadBuf read done, change to next buf
			buf = buf->next;
			run->curReadBuf = buf;
			buf->size4Read = 0;
			bufLeft = buf->cap;
			run->readBufIdx.fetch_add(1, std::memory_order_release);
		}
		if (readable < bufLeft) {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, readable, run->udRead);
			buf->size4Read += readable;
			writeCnt += readable;
			readable = 0;
		}
		else {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, bufLeft, run->udRead);
			buf->size4Read = buf->cap;
			writeCnt += bufLeft;
			readable -= bufLeft;
		}
	}
	run->readable.fetch_sub(writeCnt, std::memory_order_release);
	return 1;
}
static int dec_read_now(struct read_runtime* run, int* waitReadable, int* trunc)
{
	int readableOri = run->readable.load(std::memory_order_acquire);
	return read_now_inner(run, readableOri, waitReadable, trunc);
}
static int dec_read_len(struct read_runtime* run, int* waitReadable, int* trunc)
{
	struct read_decode_len* d = (struct read_decode_len*)run->curDec;
	int readableOri = run->readable.load(std::memory_order_acquire);
	if (readableOri < d->readLen) {  // no readable data
		*waitReadable = d->readLen;
		return 0;
	}
	*trunc = 0;
	readableOri = d->readLen;
	struct read_buf_block* buf = run->curReadBuf;
	int writeCnt = 0;
	int readable = readableOri;
	while (readable > 0) {
		int bufLeft = buf->cap - buf->size4Read;
		if (bufLeft < 1) {   // curReadBuf read done, change to next buf
			buf = buf->next;
			run->curReadBuf = buf;
			buf->size4Read = 0;
			bufLeft = buf->cap;
			run->readBufIdx.fetch_add(1, std::memory_order_release);
		}
		if (readable < bufLeft) {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, readable, run->udRead);
			buf->size4Read += readable;
			writeCnt += readable;
			readable = 0;
		}
		else {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, bufLeft, run->udRead);
			buf->size4Read = buf->cap;
			writeCnt += bufLeft;
			readable -= bufLeft;
		}
	}
	run->readable.fetch_sub(writeCnt, std::memory_order_release);
	return 1;
}
static int read_sep_inner(struct read_runtime* run, int* waitReadable, int readableOri)
{
	struct read_decode_sep* d = (struct read_decode_sep*)run->curDec;
	int unseekBytes = readableOri - d->seekedBytes;
	if (unseekBytes < d->sepLen - d->sepCharSeek) {
		*waitReadable = readableOri + d->sepLen - d->sepCharSeek;
		return 0;
	}
	//seek
	bool found = false;
	struct read_buf_block* buf = d->seekBuf;
	while (--unseekBytes >= 0) {
		if (d->seekBufPos >= buf->cap) {  // seek next buf
			buf = buf->next;
			d->seekBuf = buf;
			d->seekBufPos = 0;
		}
		char ch1 = buf->buf[d->seekBufPos++];
		char ch2 = d->sep[d->sepCharSeek];
		if (ch1 == ch2) {
			++d->sepCharSeek;
		}
		else {
			d->sepCharSeek = 0;
		}
		++d->seekedBytes;
		if (d->sepCharSeek == d->sepLen) {   // match found
			found = true;		
			break;
		}
	}
	if (!found) {  // match not found
		*waitReadable = readableOri + d->sepLen - d->sepCharSeek;
		return 0;
	}
	if(d->seekedBytes > run->readPackMax){  // need trunc
		return 0;
	}
	// consume data
	readableOri = d->seekedBytes - d->sepLen;
	buf = run->curReadBuf;
	int readable = readableOri;
	if (readable > 0) {
		int writeCnt = 0;
		while (readable > 0) {
			int bufLeft = buf->cap - buf->size4Read;
			if (bufLeft < 1) { // curReadBuf read done, change to next buf
				buf = buf->next;
				run->curReadBuf = buf;
				buf->size4Read = 0;
				bufLeft = buf->cap;
				run->readBufIdx.fetch_add(1, std::memory_order_release);
			}
			if (readable < bufLeft) {
				run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, readable, run->udRead);
				buf->size4Read += readable;
				writeCnt += readable;
				readable = 0;
			}
			else {
				run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, bufLeft, run->udRead);
				buf->size4Read = buf->cap;
				writeCnt += bufLeft;
				readable -= bufLeft;
			}
		}
	}
	else {
		run->cbRead(0, 0, nullptr, 0, run->udRead);
	}
	// consume sep
	buf = run->curReadBuf;
	readable = d->sepLen;
	while (readable > 0) {
		int bufLeft = buf->cap - buf->size4Read;
		if (bufLeft < 1) {  // curReadBuf read done, change to next buf
			buf = buf->next;
			run->curReadBuf = buf;
			buf->size4Read = 0;
			bufLeft = buf->cap;
			run->readBufIdx.fetch_add(1, std::memory_order_release);
		}
		if (readable < bufLeft) {
			buf->size4Read += readable;
			readable = 0;
		}
		else {
			buf->size4Read = buf->cap;
			readable -= bufLeft;
		}
	}
	run->readable.fetch_sub(d->seekedBytes, std::memory_order_release);
	//d->sepCharSeek = 0;
	//d->seekedBytes = 0;
	return 1;
}
static int dec_read_sep(struct read_runtime* run, int* waitReadable, int* trunc)
{
	int readableOri = run->readable.load(std::memory_order_acquire);
	if(read_sep_inner(run, waitReadable, readableOri)){ // read succ & no trunc
		*trunc = 0;   // no trunc
		return 1;
	}
	// read nothing, check trunc
	if(readableOri > run->readPackMax){   // can trunc
		assert(read_now_inner(run, readableOri, waitReadable, trunc));   
		assert(*trunc == 1);  //  debug
		return 1;
	}
	return 0;
}
static int dec_read_all(struct read_runtime* run, int* waitReadable, int* preBytes)
{
	return 1;
}
static inline void dec_init(struct read_runtime* run, struct read_arg* arg, int decType)
{
	struct read_decode* d = run->arrDecode[decType];
	d->session = arg->session;
	d->type = decType;
	run->curDec = d;
}
static inline int dec_init_now(struct read_runtime* run, struct read_arg* arg)
{
	if (run->arrDecode[DECTYPE_NOW] == nullptr) {
		struct read_decode_now* d = new struct read_decode_now;
		run->arrDecode[DECTYPE_NOW] = (struct read_decode*)d;
	}
	dec_init(run, arg, DECTYPE_NOW);
	return 1;
}
static inline int dec_init_len(struct read_runtime* run, struct read_arg* arg)
{
	if (run->arrDecode[DECTYPE_LEN] == nullptr) {
		struct read_decode_len* d = new struct read_decode_len;
		run->arrDecode[DECTYPE_LEN] = (struct read_decode*)d;
	}
	dec_init(run, arg, DECTYPE_LEN);
	//
	struct dec_arg_len* da = (struct dec_arg_len*)arg->decArg;
	struct read_decode_len* d = (struct read_decode_len*)run->arrDecode[DECTYPE_LEN];
	d->readLen = da->readLen;
	return 1;
}
static inline int dec_init_sep(struct read_runtime* run, struct read_arg* arg)
{
	if (run->arrDecode[DECTYPE_SEP] == nullptr) {
		struct read_decode_sep* d = new struct read_decode_sep;
		run->arrDecode[DECTYPE_SEP] = (struct read_decode*)d;
		d->sep = nullptr;
		d->sepCap = 0;
	}
	dec_init(run, arg, DECTYPE_SEP);
	//
	struct dec_arg_sep* da = (struct dec_arg_sep*)arg->decArg;
	struct read_decode_sep* d = (struct read_decode_sep*)run->arrDecode[DECTYPE_SEP];
	if (d->sepCap > 0 && da->sepLen > d->sepCap) {
		pps_free(d->sep);
		d->sep = nullptr;
		d->sepCap = 0;
	}
	if (d->sep == nullptr) {
		d->sepCap = da->sepLen;
		d->sep = (char*)pps_malloc(d->sepCap);
	}
	d->seekedBytes = 0;
	d->sepCharSeek = 0;
	d->sepLen = da->sepLen;
	//
	d->seekBuf = run->curReadBuf;
	d->seekBufPos = run->curReadBuf->size4Read;
	memcpy(d->sep, da->sep, d->sepLen);
	return 1;
}
static inline int dec_init_all(struct read_runtime* run, struct read_arg* arg)
{
	if (run->arrDecode[DECTYPE_ALL] == nullptr) {
		struct read_decode_all* d = new struct read_decode_all;
		run->arrDecode[DECTYPE_ALL] = (struct read_decode*)d;
	}
	dec_init(run, arg, DECTYPE_ALL);
	return 1;
}

// enreq
static inline void enreq_read_over(struct net_task_req* t, struct netreq_read_wait* req)
{
	t->src = req->src;
	t->cmd = NETCMD_READ_OVER;
	int sz = sizeof(struct tcp_read_wait);
	assert(sz <= TASK_REQ_BUF_LEN);
	t->szBuf = sz;
	memcpy(t->buf, req->wait, sz);
}
static inline void enreq_read_wait(struct net_task_req* t, struct netreq_read_wait* req)
{
	t->src = req->src;
	t->cmd = NETCMD_READ_WAIT;
	int sz = sizeof(struct tcp_read_wait);
	assert(sz <= TASK_REQ_BUF_LEN);
	t->szBuf = sz;
	memcpy(t->buf, req->wait, sz);
}
static inline void enreq_send_wait(struct net_task_req* t, struct netreq_send_wait* req)
{
	t->src = req->src;
	t->cmd = NETCMD_SEND_WAIT;
	t->szBuf = 0;
}
static inline void enreq_tcp_close(struct net_task_req* t, struct netreq_tcp_close* req)
{
	t->src.idx = req->sockId.idx;
	t->src.cnt = req->sockId.cnt;
	t->cmd = NETCMD_TCP_CLOSE;
	t->szBuf = 0;
}
static inline void enreq_tcp_add(struct net_task_req* t, struct netreq_tcp_add* req)
{
	t->src.idx = req->sockId.idx;
	t->src.cnt = req->sockId.cnt;
	t->cmd = NETCMD_TCP_ADD;
	size_t sz = sizeof(struct tcp_add_info);
	assert(sz <= TASK_REQ_BUF_LEN);
	t->szBuf = sz;
	memcpy(t->buf, req->info, sz);
}
static void enreq_tcp_listen(struct net_task_req* t, struct netreq_tcp_listen* req)
{
	t->src = req->src;
	t->cmd = NETCMD_TCP_LISTEN;
	size_t sz = sizeof(struct tcp_server_cfg);
	assert(sz <= TASK_REQ_BUF_LEN);
	t->szBuf = sz;
	memcpy(t->buf, req->cfg, sz);
}
template<typename T>
static void check_range(T& v, T min, T max)
{
	if (v < min) { v = min;}
	if (v > max) { v = max;}
}
int net_tcp_listen(struct netreq_src* src, struct pipes* pipes, struct tcp_server_cfg* cfg)
{	
	//check cfg
	int addrNum = cfg->addrNum;
	if (addrNum < 1) {
		return 2;
	}
	for (int i = 0; i < addrNum; ++i) {
		if (i > 0) {
			if (sock_addr_equal(&cfg->addrs[i], &cfg->addrs[i-1])) {
				return 3;   // addr duplicate
			}
		}
		if (addrNum > 1 && sock_addr_isinany(&cfg->addrs[i])) {
			return 4;
		}
	}
	check_range(cfg->sendBuf, SOCK_SEND_BUF_MIN, SOCK_SEND_BUF_MAX);
	check_range(cfg->recvBuf, SOCK_RECV_BUF_MIN, SOCK_RECV_BUF_MAX);
	if (cfg->backlog < 1) {
		cfg->backlog = SOCK_BACKLOG_DEF;
	}
	else if (cfg->backlog > SOCK_BACKLOG_MAX) {
		cfg->backlog = SOCK_BACKLOG_MAX;
	}
	//
	struct netreq_tcp_listen req;
	req.src = *src;
	req.cfg = cfg;
	send_to_net(alloc_net(pipes), &req, enreq_tcp_listen, nullptr);
	return 0;
}


static void enreq_net_shutdown(struct net_task_req* t, struct netreq_tcp_shutdown* req)
{
	t->cmd = NETCMD_SHUTDOWN;
	t->szBuf = 0;
}
void net_shutdown(struct pipes* pipes)
{
	if(pipes->config->net_num > 0){  // net loop exist
		for(int i=0; i<pipes->config->net_num; ++i){
			struct netreq_tcp_shutdown req;
			send_to_net(&pipes->nets[i], &req, enreq_net_shutdown, nullptr);
		}
	}
}

//
struct conn_job
{
	int retGotByCtrl;
	int done;
	int ret;
	int inProgress;
	SOCK_FD fd;
	SOCK_ADDR addrRemote;
	std::mutex mtx;
};
struct conn_ctrl_arg
{
	struct netreq_src src;
	struct tcp_connect_cfg* cfg;
	int64_t tmStart;
	std::mutex mtx;
	struct conn_job* job;
};
static void conn_job_thread(void* ud)
{
	struct conn_ctrl_arg* ctrl = (struct conn_ctrl_arg*)ud;
	// copy conn cfg
	struct tcp_connect_cfg* connCfg = new tcp_connect_cfg;
	*connCfg = *ctrl->cfg;
	size_t hostLen = strlen(ctrl->cfg->host);
	connCfg->host = (char*)pps_malloc(hostLen + 1);
	strcpy(connCfg->host, ctrl->cfg->host);
	// init job
	struct conn_job* job = new struct conn_job;
	{
		std::lock_guard<std::mutex> lock(job->mtx);
		job->done = 0;
		job->retGotByCtrl = 0;
	}
	{  // job init done
		std::lock_guard<std::mutex> lock(ctrl->mtx);
		ctrl->job = job;
	}
	// do conn
	SOCK_FD fd;
	SOCK_ADDR addrRemote;
	int inProgress = 0;
	printf("conn_job_thread, conn start\n"); // debug
	int ret = sock_tcp_connect_nonblock(connCfg, &fd, &addrRemote, &inProgress);
	{   // conn has done
		std::lock_guard<std::mutex> lock(job->mtx);
		job->done = 1;
		job->ret = ret;
		job->fd = fd;
		job->inProgress = inProgress;
		job->addrRemote = addrRemote;
	}
	printf("conn_job_thread, conn done\n"); // debug
	// wait enough time for ctrlThread to get result
	std::this_thread::sleep_for(std::chrono::seconds(10));
	printf("conn_job_thread, finalize\n"); // debug
	if(ret == 0){  // fd valid, check if got by ctrl
		bool closeFd = false;
		{
			std::lock_guard<std::mutex> lock(job->mtx);
			if(!job->retGotByCtrl){
				closeFd = true;
			}
		}
		if(closeFd){
			sock_fd_close(&fd);
		}
	}
	// finalize
	pps_free(connCfg->host);
	delete connCfg;
	delete job;
}
static void enreq_tcp_connect(struct net_task_req* t, struct netreq_tcp_conn* req);
static void conn_ctrl_thread(struct pipes* pipes, void* ud)
{
	struct conn_ctrl_arg* arg = (struct conn_ctrl_arg*)ud;
	struct tcp_connect_cfg* connCfg = arg->cfg;
	{
		std::lock_guard<std::mutex> lock(arg->mtx);
		arg->job = nullptr;
	}
	// start job thread
	std::thread thJob = std::thread(conn_job_thread, arg);
	thJob.detach();
	// wait job thread init done
	struct conn_job* job = nullptr;
	while(true){
		{
			std::lock_guard<std::mutex> lock(arg->mtx);
			if(arg->job){   // job thread init done
				job = arg->job;
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	//  wait jobDone or timeout
	int ret = SOCK_ERR_CONN_FAILED;
	int inProgress = 0;
	SOCK_FD fd;
	SOCK_ADDR addrRemote;
	while(true){
		if(pipes->hasShutdown.load(std::memory_order_relaxed)){   // pipes shutdown called
			break;
		}
		{
			std::lock_guard<std::mutex> lock(job->mtx);
			if(job->done){  // conn done
				ret = job->ret;
				inProgress = job->inProgress;
				fd = job->fd;
				addrRemote = job->addrRemote;
				job->retGotByCtrl = 1;
				break;
			}
		}
		if(connCfg->timeout > 0){   // timeout specify, check
			int64_t tmNow = timer_clock_now_ms(pipes);
			if(tmNow >= arg->tmStart + connCfg->timeout){   // timedout
				ret = SOCK_ERR_TIMEOUT;
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("conn_ctrl_thread, ret=%d, inProgress=%d\n", ret, inProgress);     // debug
	if(ret == 0){    //  conn succ or inprogress
		struct tcp_conn_info info;
		info.fd = fd;
		info.sendbuf = connCfg->sendBuf;
		info.recvbuf = connCfg->recvBuf;
		info.portRemote = connCfg->port;
		info.addrRemote = addrRemote;
		info.inProgress = inProgress;
		info.timeout = connCfg->timeout;
		if(info.timeout > 0){   // timeout specify
			info.expiration = info.timeout + arg->tmStart;
		}
		struct netreq_tcp_conn req;
		req.src = arg->src;
		req.info = &info;
		send_to_net(alloc_net(pipes), &req, enreq_tcp_connect, nullptr);
	}else{   // conn failed
		struct tcp_conn_ret connRet;
		connRet.ret = ret;
		struct net_msg_ori netMsgOri;
		// notify svc
		wrap_svc_msg(&netMsgOri, &arg->src, NETCMD_TCP_CONNECT, &connRet, sizeof(struct tcp_conn_ret));
		send_to_svc_ext(&netMsgOri, pipes);	
	}
	// release
	pps_free(arg->cfg->host);
	delete arg->cfg;
	delete arg;
	return;
}
static void enreq_tcp_connect(struct net_task_req* t, struct netreq_tcp_conn* req)
{
	t->src = req->src;
	t->cmd = NETCMD_TCP_CONNECT;
	size_t sz = sizeof(struct tcp_conn_info);
	assert(sz <= TASK_REQ_BUF_LEN);
	t->szBuf = sz;
	memcpy(t->buf, req->info, sz);
}
int net_tcp_connect(struct netreq_src* src, struct pipes* pipes, struct tcp_connect_cfg* cfg)
{
	check_range(cfg->timeout, 0, SOCK_CONN_TIMEOUT_MAX);
	struct tcp_connect_cfg* cfgCp = new struct tcp_connect_cfg;
	*cfgCp = *cfg;
	size_t hostLen = strlen(cfg->host);
	cfgCp->host = (char*)pps_malloc(hostLen + 1);
	strcpy(cfgCp->host, cfg->host);
	//
	struct conn_ctrl_arg* arg = new struct conn_ctrl_arg;
	arg->src = *src;
	arg->cfg = cfgCp;
	if(cfg->timeout > 0){   // timeout specify, rec tmStart
		arg->tmStart = timer_clock_now_ms(pipes);
	}
	//
	pps_ext_thread(pipes, conn_ctrl_thread, arg);
	return 1;
}

static void fn_pop_net_req(struct net_task_req* src, struct net_task_req* dst)
{
	dst->src = src->src;
	dst->cmd = src->cmd;
	dst->szBuf = src->szBuf;
	if(src->szBuf > 0){
		memcpy(dst->buf, src->buf, src->szBuf);
	}
}
//
static int conn_wait_compare(struct conn_wait_info* v1, struct conn_wait_info* v2)
{
	if (v1->expiration < v2->expiration)
	{
		return -1;
	}
	if (v1->expiration > v2->expiration)
	{
		return 1;
	}
	return 0;
}
int net_init_main_thread(struct pps_net* net, struct pipes* pipes, struct socket_mgr* sockMgr, uint32_t index)
{
	net->index = index;
	net->pollWait = -1;  // means pollWait block indefinitely
	net->pipes = pipes;
	net->sockMgr = sockMgr;
	//
	net->sockCtxEvent.sockCap = 0;
	net->sockCtxEvent.socks = nullptr;
	net->sockCtxEvent.sockNum = 0;
	sock_ctxsocks_init(&net->sockCtxEvent, 0);

	if(!sock_pollfd_new(&net->pollFd)){
		return 0;
	}
	if(!sock_eventfd_new(net->pollFd, &net->eventFd)){
		return 0;
	}
	//
	net->queTaskReq = const_cast<struct mq_mpsc<struct net_task_req>*>(mpsc_create<struct net_task_req>(65536));
	sock_poll_runtime_init(&net->pollRuntime);
	//
	net->queUnSendMsg = const_cast<pool_linked<struct net_msg_ori>*>(plk_create<struct net_msg_ori>(2048));
	net->unSendMsgNum = 0;
	//
	net->queWaitRecycleSock = const_cast<pool_linked<int32_t>*>(plk_create<int32_t>(2048));
	net->waitRecycleSockNum = 0;
	//
	net->cbTcpAccept = on_tcp_accept;
	net->cbTcpRead = on_tcp_read;
	net->cbTcpSend = on_tcp_send;
	net->cbTcpConnWait = on_tcp_conn_done;
	//
	net->pNetMsgOri = &net->netMsgOri;
	//
	net->queConnTimeout = minheap_create<struct conn_wait_info*>(1024, conn_wait_compare);
	net->connTimeoutNum = 0;
	//
	net->pSetFd = new std::unordered_set<SOCK_FD>();
	return 1;
}

void net_deinit_main_thread(struct pps_net* net)
{
	sock_eventfd_destroy(net->eventFd);
	sock_pollfd_destroy(net->pollFd);
	mpsc_destroy(net->queTaskReq);
	sock_poll_runtime_deinit(net->pollRuntime);
	plk_destroy(net->queUnSendMsg);
	delete net->pSetFd;
	minheap_destroy(net->queConnTimeout);
}

