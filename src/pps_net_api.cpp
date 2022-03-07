#include "pps_net_api.h"
#include "pps_socket.h"
#include "pps_net.h"
#include "pps_timer.h"
#include "pps_service.h"
#include "pps_socket_define.h"

static FN_READDEC_INIT s_arrDecInit[TCP_DEC_NUM] = { sockdec_init_now, sockdec_init_len, sockdec_init_sep };
static FN_READDEC_READ s_arrDecRead[TCP_DEC_NUM] = { sockdec_read_now, sockdec_read_len, sockdec_read_sep };

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

template<typename T>
static void check_range(T& v, T min, T max)
{
	if (v < min) { v = min; }
	if (v > max) { v = max; }
}
static void enreq_tcp_listen(struct net_task_req* t, struct netreq_tcp_listen* req);
int netapi_tcp_listen(struct netreq_src* src, struct pipes* pipes, struct tcp_server_cfg* cfg)
{
	//check cfg
	int addrNum = cfg->addrNum;
	if (addrNum < 1) {
		return 2;
	}
	for (int i = 0; i < addrNum; ++i) {
		if (i > 0) {
			if (sock_addr_equal(&cfg->addrs[i], &cfg->addrs[i - 1])) {
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
	} else if (cfg->backlog > SOCK_BACKLOG_MAX) {
		cfg->backlog = SOCK_BACKLOG_MAX;
	}
	//
	struct netreq_tcp_listen req;
	req.src = *src;
	req.cfg = cfg;
	send_to_net(net_alloc(pipes), &req, enreq_tcp_listen, nullptr);
	return 0;
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
int netapi_tcp_read(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt, struct read_arg* arg, int*trunc)
{
	struct socket_ctx* ctx = sock_valid_ctx(pipes->sockMgr, sockIdx, sockCnt);
	if (ctx == nullptr) {
		return -1;
	}
	struct read_runtime* run = &ctx->readRuntime;
	if (!sock_read_trylock(ctx)) {   //  concurrent
		while (!sock_read_trylock(ctx)) {
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
		if (!hasSendReadOver) {   // send read over
			send_read_over(net_get(pipes, ctx->idxNet), sockIdx, sockCnt, arg);
		}
		return -1;
	}
	run->preLockOwner = 1;   //  mark lock by read()
	int decType = -1;
	if (run->onReadWait) {    // onReadWait
		if (arg->isNewRead) {
			sock_read_unlock(ctx);
			return -3;
		}
		decType = run->curDec->type;
	} else {  // not onRead
		if (!arg->isNewRead) { //  arg invalid
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
	} else {   // readWait
		if (run->sockClosed) {   // read sock closed, do last read
			run->onReadWait = false;
			/*
			ret = s_arrDecRead[decType](run, &waitReadable, trunc);   // try again, the buf may recv more data after "readAbove"
			if(ret) {   //read succ
			sock_read_unlock(ctx);
			return 1;
			}
			*/
			if (decType != DECTYPE_NOW) {   // read all left
				ret = s_arrDecRead[DECTYPE_NOW](run, &waitReadable, trunc);
			}
			run->sockCnt = ctx->cnt4Net + 1;        // incr sockCnt to forbid follow read()
													// tell net read() over
			bool hasSendReadOver = run->hasSendReadOver;
			run->hasSendReadOver = true;
			sock_read_unlock(ctx);
			if (!hasSendReadOver) {
				send_read_over(net_get(pipes, ctx->idxNet), sockIdx, sockCnt, arg);
			}
			if (ret) {   // read sth
				return 2;    // means last read
			} else {  // no data has read
				return -1;     // means sock closed
			}
		} else {    //  not read anything, send readWait req to net
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

static inline void enreq_send_wait(struct net_task_req* t, struct netreq_send_wait* req);
int netapi_tcp_send(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt, const char* data, int size)
{
	struct socket_ctx* ctx = sock_valid_ctx(pipes->sockMgr, sockIdx, sockCnt);
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
	if (run->closeCalled4Svc) {   // has called close
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
		if (hasSend == SEND_RET_AGAIN) {// send nothing, sendbuf is full
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
			} else {
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
		} else {
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
int netapi_close_sock(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt)
{
	struct socket_ctx* ctx = sock_valid_ctx(pipes->sockMgr, sockIdx, sockCnt);
	if (ctx == nullptr) {
		return 0;
	}
	struct send_runtime* run = &ctx->sendRuntime;
	if (!sock_send_trylock(ctx)) {  // concurrent
		while (!sock_send_trylock(ctx)) {
		}
		if (sockCnt != run->sockCnt) {  // sockCtx has released
			sock_send_unlock(ctx);
			return 0;
		}
	}
	run->preLockOwner = 2;    // mark lock by close()
	if (run->closeCalled4Svc) {  //  close() has called
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
	if (ret == 0) {  // fd valid, check if got by ctrl
		bool closeFd = false;
		{
			std::lock_guard<std::mutex> lock(job->mtx);
			if (!job->retGotByCtrl) {
				closeFd = true;
			}
		}
		if (closeFd) {
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
	while (true) {
		{
			std::lock_guard<std::mutex> lock(arg->mtx);
			if (arg->job) {   // job thread init done
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
	while (true) {
		if (pipes->hasShutdown.load(std::memory_order_relaxed)) {   // pipes shutdown called
			break;
		}
		{
			std::lock_guard<std::mutex> lock(job->mtx);
			if (job->done) {  // conn done
				ret = job->ret;
				inProgress = job->inProgress;
				fd = job->fd;
				addrRemote = job->addrRemote;
				job->retGotByCtrl = 1;
				break;
			}
		}
		if (connCfg->timeout > 0) {   // timeout specify, check
			int64_t tmNow = timer_clock_now_ms(pipes);
			if (tmNow >= arg->tmStart + connCfg->timeout) {   // timedout
				ret = SOCK_ERR_TIMEOUT;
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("conn_ctrl_thread, ret=%d, inProgress=%d\n", ret, inProgress);     // debug
	if (ret == 0) {    //  conn succ or inprogress
		struct tcp_conn_info info;
		info.fd = fd;
		info.sendbuf = connCfg->sendBuf;
		info.recvbuf = connCfg->recvBuf;
		info.portRemote = connCfg->port;
		info.addrRemote = addrRemote;
		info.inProgress = inProgress;
		info.timeout = connCfg->timeout;
		if (info.timeout > 0) {   // timeout specify
			info.expiration = info.timeout + arg->tmStart;
		}
		struct netreq_tcp_conn req;
		req.src = arg->src;
		req.info = &info;
		send_to_net(net_alloc(pipes), &req, enreq_tcp_connect, nullptr);
	} else {   // conn failed
		struct tcp_conn_ret connRet;
		connRet.ret = ret;
		struct net_msg_ori netMsgOri;
		// notify svc
		net_wrap_svc_msg(&netMsgOri, &arg->src, NETCMD_TCP_CONNECT, &connRet, sizeof(struct tcp_conn_ret));
		net_send_to_svc_ext(&netMsgOri, pipes);
	}
	// release
	pps_free(arg->cfg->host);
	delete arg->cfg;
	delete arg;
	return;
}
int netapi_tcp_connect(struct netreq_src* src, struct pipes* pipes, struct tcp_connect_cfg* cfg)
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
	if (cfg->timeout > 0) {   // timeout specify, rec tmStart
		arg->tmStart = timer_clock_now_ms(pipes);
	}
	//
	pps_ext_thread(pipes, conn_ctrl_thread, arg);
	return 1;
}


int netapi_get_remote(struct pipes* pipes, int idx, uint32_t cnt, char* buf, int szBuf, int* port)
{
	struct socket_ctx* ctx = sock_valid_ctx(pipes->sockMgr, idx, cnt);
	if (ctx == nullptr) {
		return 0;
	}
	if (!sock_addr_ntop(&ctx->addrRemote, buf, szBuf)) {
		return 0;
	}
	*port = ctx->portRemote;
	return 1;
}
int netapi_is_listen_valid(struct pipes* pipes, int32_t sockIdx, uint32_t sockCnt)
{
	struct socket_ctx* ctx = sock_valid_ctx(pipes->sockMgr, sockIdx, sockCnt);
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

//
static void enreq_tcp_listen(struct net_task_req* t, struct netreq_tcp_listen* req)
{
	t->src = req->src;
	t->cmd = NETCMD_TCP_LISTEN;
	size_t sz = sizeof(struct tcp_server_cfg);
	assert(sz <= TASK_REQ_BUF_LEN);
	t->szBuf = sz;
	memcpy(t->buf, req->cfg, sz);
}
static inline void enreq_tcp_close(struct net_task_req* t, struct netreq_tcp_close* req)
{
	t->src.idx = req->sockId.idx;
	t->src.cnt = req->sockId.cnt;
	t->cmd = NETCMD_TCP_CLOSE;
	t->szBuf = 0;
}
static inline void enreq_send_wait(struct net_task_req* t, struct netreq_send_wait* req)
{
	t->src = req->src;
	t->cmd = NETCMD_SEND_WAIT;
	t->szBuf = 0;
}
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
static void enreq_tcp_connect(struct net_task_req* t, struct netreq_tcp_conn* req)
{
	t->src = req->src;
	t->cmd = NETCMD_TCP_CONNECT;
	size_t sz = sizeof(struct tcp_conn_info);
	assert(sz <= TASK_REQ_BUF_LEN);
	t->szBuf = sz;
	memcpy(t->buf, req->info, sz);
}