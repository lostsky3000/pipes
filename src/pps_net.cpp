#include "pps_net.h"
#include "pipes.h"
#include "pps_socket.h"
#include "pps_socket_define.h"
#include "pps_malloc.h"
#include "tcp_protocol.h"
#include "pps_net_api.h"
#include "pps_service.h"
#include "net_helper.h"
#include <thread>
#include <cassert>

//
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
static void fn_pop_net_req(struct net_task_req* src, struct net_task_req* dst);
static void send_readnotify_to_svc(struct pps_net* net, struct netreq_src* src, struct tcp_read_wait_ret* ret)
{
	net_wrap_svc_msg(net->pNetMsgOri, src, NETCMD_READ_WAIT, ret, sizeof(struct tcp_read_wait_ret));
	int sendRet = net_send_to_svc(net->pNetMsgOri, net, true);
	if(sendRet < 0){   // svc has gone, do readOver
		struct read_arg arg;
		arg.srcIdx = src->idx;
		arg.srcCnt = src->cnt;
		arg.session = src->session;
		netapi_send_read_over(net, ret->sockId.idx, ret->sockId.cnt, &arg);
	}
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

static int conn_rsp(void* ud, const char* buf, int sz)
{
	SOCK_FD* pFd = (SOCK_FD*)ud;
	int ret = sock_tcp_send(*pFd, buf, sz);
	if(ret >= sz){   // all send done
		return 1;
	}
	if(ret == SEND_RET_CLOSE){  // conn closed
		return 0;
	}
	// sendBuf is full, rarely
	int hasSend = 0;
	if(ret != SEND_RET_AGAIN){   // has send some
		sz -= ret;
		hasSend += ret;
	}
	while(sz > 0){  // loop send
		ret = sock_tcp_send(*pFd, buf + hasSend, sz);
		if (ret == SEND_RET_CLOSE) {  // conn closed
			return 0;
		}
		if(ret != SEND_RET_AGAIN){
			sz -= ret;
			hasSend += ret;
		}
	}
	return 1;
}
static int pack_inner_rsp(void* ud, const char* buf, int sz)
{
	return 1;
}
static inline bool notify_svc_conn_in(struct pps_net* net, struct socket_ctx* ctx);
static int on_tcp_read(struct pps_net* net, struct socket_sockctx* sock)
{
	struct socket_ctx* ctx = sock->main; 
	struct read_runtime* run = &ctx->readRuntime;
	bool& rdSockClosed = run->sockClosed;
	int readable = 0;
	int readable4Dec = 0;
	if (!rdSockClosed) {
		struct read_buf_block* buf;
		// recycle bufs that has read
		uint32_t readBufIdx = run->readBufIdx.load(std::memory_order_acquire);
		while ((buf = rdbuf_front(&run->queReading))) {
			if (buf->idx == readBufIdx) {
				break;	
			}
			buf = rdbuf_pop(&run->queReading);
			rdbuf_push(&run->queFree, buf);
		}
		// do fill
		buf = run->curFillBuf;
		if (buf->size4Fill >= buf->cap) {   // curFillBuf is full, prepare newBuf for fill
			buf = expand_fill_buf(run, ctx->recvBufLen);
		}
		int read;
		while (true) {
			read = sock_tcp_read(sock->fd, buf->buf + buf->size4Fill, buf->cap - buf->size4Fill);
			if (read > 0) {   // read some
				buf->size4Fill += read;
				readable = run->readable.fetch_add(read, std::memory_order_release) + read;
				if(run->protocolNeedDecode){
					readable4Dec = run->readable4Dec.fetch_add(read, std::memory_order_release) + read;
				}
				if (buf->size4Fill >= buf->cap) {  // cur buf is full, expand
					buf = expand_fill_buf(run, ctx->recvBufLen);
				}
			} else {  //  no more data or conn has gone
				if(read != READ_RET_AGAIN) {  // close or halfClose
					rdSockClosed = true;
					sock_read_atom_release(ctx);        // make sockClosed visible for svc read
					//if(read == READ_RET_HALF_CLOSE && ctx->pollOutReg) {  // half close & has unsend data 
					if (read == READ_RET_HALF_CLOSE){
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
	int32_t& waitReadable = run->waitReadable;
	bool hasReadWait = (waitReadable > 0);
	bool readWaitTrig = false;
	if(!rdSockClosed){
		if(ctx->hasProtocol){
			struct tcp_decode* decTcp = run->tcpDec;
			if(decTcp->state == TCPDEC_STATE_CONN_CHECKING){  // conn not done, just read for checking conn
				//sock_read_atom_acquire(ctx);
				int connRet = sockprotocol_read_conn(run, readable, decTcp);
				if(connRet == 0){   // init packread
					decTcp->cbPackReset(decTcp);
				}
				sock_read_atom_release(ctx);
				if(connRet > -1){  // connDone, rsp to client
					if(connRet > 0 || !decTcp->cbConnRsp(decTcp, &sock->fd, conn_rsp)
							|| !notify_svc_conn_in(net, ctx)){
						// conn check failed || send rsp failed || notify svc failed(svc gone), close & recycle
						close_ctx_fd(net, ctx);
						recycle_ctx(net, ctx);
						return 0;
					}
				}
				return 1;
			}
			if(hasReadWait){   // has readWait check
				if (decTcp->state == TCPDEC_STATE_PACK_HEAD) {
					int headRet = sockprotocol_pack_head(run, readable, decTcp);
					if (headRet == 0) {   // parse packhead done, sync decode&readbuf
						run->curReadBuf->size4Decode = run->curReadBuf->size4Read;
						run->curDecodeBuf = run->curReadBuf;
					}else if (headRet > 0) {   // parse pack-head error
						// mark close
						rdSockClosed = true;
						ctx->pollInReg = false;
						ctx->pollOutReg = false;
						sock_poll_del_fd(net->pollFd, sock);         // remove events reg
					}
				}
				if(decTcp->state == TCPDEC_STATE_INNER_MSG){   // got inner msg
					readWaitTrig = true;
				}
				if (decTcp->state == TCPDEC_STATE_PACK_DEC_BODY && run->protocolNeedDecode) {
					int bodyRet = sockprotocol_pack_dec_body(run, readable4Dec, decTcp);
					if (bodyRet == 0) {   // parse body done(succ)
						readWaitTrig = true;
					}else if (bodyRet > 0) {   // parse body error
						// mark close
						rdSockClosed = true;
						ctx->pollInReg = false;
						ctx->pollOutReg = false;
						sock_poll_del_fd(net->pollFd, sock);         // remove events reg
					}
				}
				sock_read_atom_release(ctx);
			}
		}else{   // no protocol
			if (hasReadWait) {  // has readwait task && not trig yet, check read wait trig
				if (run->readWaitDecType == DECTYPE_SEP) {
					struct read_decode_sep* dec = (struct read_decode_sep*)run->curDec;
					if (sockdec_sep_seek(dec, readable)) {
						readWaitTrig = true;
					}else{   //check if packbuf is too long
						if(readable >= run->readPackMax){   // pack too long
							// mark close
							rdSockClosed = true;
							ctx->pollInReg = false;
							ctx->pollOutReg = false;
							sock_poll_del_fd(net->pollFd, sock);         // remove events reg
						}
					}
					sock_read_atom_release(ctx);
				} else {
					if (readable >= waitReadable) {
						readWaitTrig = true;
					}
				}
			}
		}
		// check pause read
		if(readable >= ctx->recvBufLen && (!hasReadWait || readWaitTrig) && !rdSockClosed){
			ctx->pollInReg = false;
			poll_evt_mod(net->pollFd, sock);
			printf("on_tcp_read(), readBuf is full, remove pollIn\n");         // debug
		}
	}
	if(hasReadWait){    // has readWait
		if (readWaitTrig || rdSockClosed) {   // notify svc
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
				ret = net_send_to_svc(pMsg, net, false);
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
				if(expired || waitTaskDone){   // expire or connHasDone?? remove
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
						net_wrap_svc_msg(net->pNetMsgOri, &pConnWait->ctx->src, NETCMD_TCP_CONNECT, &connRet, sizeof(struct tcp_conn_ret));
						net_send_to_svc(net->pNetMsgOri, net, true);
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

static inline bool notify_svc_conn_in(struct pps_net* net, struct socket_ctx* ctx)
{
	struct tcp_conn_in m;
	m.sockId.idx = ctx->idx;
	m.sockId.cnt = ctx->cnt4Net;
	m.sockIdParent.idx = ctx->parentIdx;
	m.sockIdParent.cnt = ctx->parentCnt;
	net_wrap_svc_msg(net->pNetMsgOri, &ctx->src, NETCMD_TCP_CONNIN, &m, sizeof(m));
	if (net_send_to_svc(net->pNetMsgOri, net, true) < 0) {   // dst svc has gone
		return false;
	}
	return true;
}
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
	int nDef = run->readWaitDecType;
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
	//
	bool connDone = true;
	if(ctx->hasProtocol){  // has protocol
		struct tcp_decode* dec = run->tcpDec;
		int readBytes;
		int connRet = dec->cbConnCheck(dec, nullptr, 0, readBytes);
		if(connRet != 0){
			connDone = false;
			if(connRet > 0){   // conn error, close sock
				close_ctx_fd(net, ctx);
				recycle_ctx(net, ctx);  // recycle ctx
			}
		}
	}
	ctx->src = info->src;
	ctx->parentIdx = info->parentIdx;
	ctx->parentCnt = info->parentCnt;
	if(connDone){
		// notify src
		if(!notify_svc_conn_in(net, ctx)){
			close_ctx_fd(net, ctx);
			recycle_ctx(net, ctx);
			return false;
		}
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
	net_wrap_svc_msg(net->pNetMsgOri, src, NETCMD_TCP_LISTEN, &ret, sizeof(ret));
	net_send_to_svc(net->pNetMsgOri, net, true);
	return true;
}
static bool do_read_wait(struct netreq_src* src, struct tcp_read_wait* req, struct pps_net* net)
{
	struct socket_ctx* ctx = sock_get_ctx(net->sockMgr, req->sockId.idx);
	struct read_runtime* run = &ctx->readRuntime;
	assert(run->waitReadable == -1);   // debug
	//
	int readable = run->readable.load(std::memory_order_relaxed);
	int decType = req->decType;
	bool readWaitTrig = false;
	if(readable > req->readableUsed){   // read new data after svc send readWait, check
		if(ctx->hasProtocol){
			sock_read_atom_acquire(ctx);
			struct tcp_decode* decTcp = run->tcpDec;
			if (decTcp->state == TCPDEC_STATE_PACK_HEAD) {
				int headRet = sockprotocol_pack_head(run, readable, decTcp);
				if (headRet > 0) {   // parse pack-head error
					sock_read_atom_release(ctx);
					// do close
					return 0;
				}
				if (headRet == 0) {   // parse packhead done, sync decode&readbuf
					run->curReadBuf->size4Decode = run->curReadBuf->size4Read;
					run->curDecodeBuf = run->curReadBuf;
					/*
					if (decTcp->state == TCPDEC_STATE_CLOSE) {  // do close
						sock_read_atom_release(ctx);
						// do close
						return 0;
					}*/
				}
			}
			if(decTcp->state == TCPDEC_STATE_INNER_MSG){   // got inner msg
				readWaitTrig = true;
			}
			if (decTcp->state == TCPDEC_STATE_PACK_DEC_BODY && run->protocolNeedDecode) {
				int bodyRet = sockprotocol_pack_dec_body(run, run->readable4Dec.load(std::memory_order_relaxed), decTcp);
				if (bodyRet > 0) {   // parse body error
					sock_read_atom_release(ctx);
					// do close
					return 0;
				}
				if (bodyRet == 0) {   // parse body done(succ)
					readWaitTrig = true;
				}
			}
			sock_read_atom_release(ctx);
		}else{
			if (decType == DECTYPE_SEP) {
				sock_read_atom_acquire(ctx);
				struct read_decode_sep* dec = (struct read_decode_sep*)run->curDec;
				readWaitTrig = sockdec_sep_seek(dec, readable);
				sock_read_atom_release(ctx);
			} else {
				readWaitTrig = (req->waitReadable <= readable);
			}
		}
	}
	if (readWaitTrig || run->sockClosed || ctx->closeCalled4Net) {
		struct tcp_read_wait_ret ret;
		ret.session = src->session;
		ret.sockId.idx = req->sockId.idx;
		ret.sockId.cnt = req->sockId.cnt;
		send_readnotify_to_svc(net, src, &ret);
		return true;
	}
	run->readWaitDecType = decType;
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
	struct socket_ctx* ctx = sock_valid_ctx(net->sockMgr, src->idx, src->cnt);
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
	struct socket_ctx* ctx = sock_valid_ctx(net->sockMgr, src->idx, src->cnt);
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
		//SOCK_FD* pFd = &ctx->socks[0].fd;
		rrun->sockClosed = true;
		sock_read_atom_release(ctx);        // make sockClosed visible for svc read
		ctx->pollInReg = false;
		ctx->pollOutReg = false;
		ctx->sendRuntime.sockClosed = true;
		//sock_poll_del_fd(net->pollFd, &ctx->socks[0]);         // remove events reg
		//sock_fd_close(pFd);
		//del_net_fd(net, pFd);
		close_ctx_fd(net, ctx);
		if (rrun->waitReadable > 0) {   // there is a readWait, notify src
			rrun->waitReadable = -1;
			struct tcp_read_wait_ret ret;
			ret.session = ctx->src.session;
			ret.sockId.idx = ctx->idx;
			ret.sockId.cnt = ctx->cnt4Net;
			send_readnotify_to_svc(net, &ctx->src, &ret);
		}else{   // recycle directly
			recycle_ctx(net, ctx);
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
static void read_runtime_init(struct socket_ctx* ctx, int recvBufLen, uint32_t sockCnt, struct protocol_cfg* protocolCfg)
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
	run->curDecodeBuf = buf;
	run->waitReadable = -1;
	run->fillBufIdx4Net = 0;
	run->fillBufIdx.store(0, std::memory_order_relaxed);
	run->readBufIdx.store(0, std::memory_order_relaxed);
	run->readable.store(0, std::memory_order_relaxed);
	run->preLockOwner = 0;
	run->sockCnt = sockCnt;
	run->sockClosed = false;
	run->hasSendReadOver = false;
	run->readWaitDecType = -1;
	// init protocol decoder
	run->protocolNeedDecode = 0;
	if(protocolCfg){
		ctx->hasProtocol = 1;
		if(run->tcpDec && run->tcpDec->protocol != protocolCfg->type){  // protocol not match
			decode_destroy(run->tcpDec);
			run->tcpDec = nullptr;
		}
		if(run->tcpDec){
			decode_reset(run->tcpDec, protocolCfg);
		}else{
			run->tcpDec = decode_new(protocolCfg);
		}
		if(run->tcpDec->packNeedDecode){
			run->protocolNeedDecode = 1;
			run->readable4Dec.store(0, std::memory_order_relaxed);
		}
	}else{
		ctx->hasProtocol = 0;
	}
	sock_read_unlock(ctx);
}

static void init_tcp_channel_ctx(struct pps_net* net, struct socket_ctx* ctx, 
	int sendBuf, int recvBuf, SOCK_FD* pFd, SOCK_ADDR* pAddrRemote, int portRemote,
	struct protocol_cfg* protocolCfg=nullptr)
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
	read_runtime_init(ctx, ctx->recvBufLen, ctx->cnt4Net, protocolCfg);   // do not swap up(ensure closeCalled visible for read())
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
	struct pps_net* netDst = net_alloc(net->pipes);
	struct socket_ctx* ctxDst = sock_get_ctx(net->sockMgr, idxCtx);
	SOCK_RT_BEGIN(ctxDst);
	init_tcp_channel_ctx(netDst, ctxDst, svrCfg->sendBuf, svrCfg->recvBuf, 
		&fd, addrRemote, portRemote, svrCfg->protocolCfg);
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
		net_wrap_svc_msg(net->pNetMsgOri, &ctx->src, NETCMD_TCP_CONNECT, &m, sizeof(m));
		int sendRet = net_send_to_svc(net->pNetMsgOri, net, true);
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
		net_wrap_svc_msg(net->pNetMsgOri, src, NETCMD_TCP_CONNECT, &m, sizeof(m));
		int sendRet = net_send_to_svc(net->pNetMsgOri, net, true);
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

int net_send_to_svc(struct net_msg_ori* msg, struct pps_net* net, bool addToWait) {
	if (msg->to.session > 0) {
		msg->to.session = -msg->to.session;
	}
	int ret = svc_send_netmsg(msg->to.idx, msg, net->pipes);
	if (ret > 0) {  //send succ
		return 1;
	}
	if (ret == 0) {   // svc mq is full
		if (addToWait) {
			plk_push_ptr(net->queUnSendMsg, msg);
			++net->unSendMsgNum;
		}
		return 0;
	}
	// svc has gone 
	return ret;
}
int net_send_to_svc_ext(struct net_msg_ori* msg, struct pipes* pipes) {
	if (msg->to.session > 0) {
		msg->to.session = -msg->to.session;
	}
	int ret = svc_send_netmsg(msg->to.idx, msg, pipes);
	if (ret > 0) {  //send succ
		return 1;
	}
	if (ret == 0) {   // svc mq is full
		while ((ret = svc_send_netmsg(msg->to.idx, msg, pipes)) == 0) {

		}
	}
	return ret;
}

// enreq
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
template<typename T>
static void check_range(T& v, T min, T max)
{
	if (v < min) { v = min;}
	if (v > max) { v = max;}
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
	//
	struct net_helper* netHelper = new struct net_helper;
	nethp_init(netHelper);
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
	//
	nethp_deinit(net->netHelper);
	delete net->netHelper;
}

