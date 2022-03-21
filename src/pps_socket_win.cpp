#include "pps_socket_win.h"


#if defined(PLAT_IS_WIN)
#include "pps_net.h"
#include "pps_socket.h"
#include "pps_socket_define.h"
#include <atomic>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

static int set_non_blocking(SOCKET fd);
static void set_keepalive(SOCKET fd);
static void set_tcp_nodelay(SOCKET fd);
static void set_tcp_buf(SOCKET fd, int sendLen, int recvLen);
static inline int trans_conn_err(int err);

int sock_poll(struct pps_net* net, int delay)
{
	struct poll_ctx* poll = net->pollFd;
	fd_set* readSet = &poll->readSet;
	fd_set* sendSet = &poll->sendSet;
	fd_set* exceptSet = &poll->exceptSet;
	// zero fdset
	FD_ZERO(readSet);
	FD_ZERO(sendSet);
	FD_ZERO(exceptSet);
	int sockNumOri = poll->sockNum;
	int sockNumCnt = sockNumOri;
	// set fdset
	//printf("select_tick, setFds, sockNum=%d\n",sockNum);  // debug
	struct socket_sockctx* sock;
	for (int i = 0; i <WIN_SOCK_MAX; ++i) {
		sock = poll->arrSockCtx[i];
		if(sock){
			if(sock->main->pollInReg){   // reg pollIn
				FD_SET(sock->fd, readSet);
				//printf("select tick, setReadSet, fd=%d\n", sock->fd);  // debug
			}
			if(sock->main->pollOutReg){  // reg pollOut
				FD_SET(sock->fd, sendSet);
			}
			if (sock->type == SOCKCTX_TYPE_TCP_CONN_WAIT) {
				FD_SET(sock->fd, exceptSet);
			}
			if(--sockNumCnt < 1){
				break;
			}
		}
	}
	// select
	int ret;
	if(delay < 0){   // infinity
		ret = select(WIN_SOCK_MAX, readSet, sendSet, exceptSet, NULL);
	}else{
		struct timeval* tmOut = &poll->tmOut;
		tmOut->tv_sec = delay / 1000;
		tmOut->tv_usec = (delay % 1000) * 1000;
		ret = select(WIN_SOCK_MAX, readSet, sendSet, exceptSet, tmOut);
	}
	//printf("select tick, sockNum=%d, ret=%d, delay=%d\n", poll->sockNum, ret, delay);
	if(ret < 1){   // nothing happened
		if(ret == SOCKET_ERROR){
			int err = WSAGetLastError();
			printf("select() error: %d\n", err);  // debug
		}
		return 0;
	}
	//
	sockNumCnt = sockNumOri;
	struct ST_POLL_RUNTIME* rt = net->pollRuntime;
	for (int i = 0; i < WIN_SOCK_MAX; ++i) {
		sock = poll->arrSockCtx[i];
		if (sock == nullptr) {
			continue;
		}
		struct socket_ctx* sockMain = sock->main;
		if (sock->type == SOCKCTX_TYPE_TCP_CHANNEL) {
			if (sockMain->pollInReg && (FD_ISSET(sock->fd, readSet) || FD_ISSET(sock->fd, exceptSet))) {
				net->cbTcpRead(net, sock);
			}
			if (sockMain->pollOutReg && FD_ISSET(sock->fd, sendSet)) {
				net->cbTcpSend(net, sock);
			}
		} else if (sock->type == SOCKCTX_TYPE_EVENT) {
			if(FD_ISSET(sock->fd, readSet) ){
				char ch;
				recv(sock->fd, &ch, 1, 0);
			}
		} else if (sock->type == SOCKCTX_TYPE_TCP_LISTEN) {
			int cnt = 0;
			SOCKET fd;
			while ((fd = accept(sock->fd, (struct sockaddr*)&rt->inAddrCli, &rt->addrLenCli)) != INVALID_SOCKET) {
				if (!net->cbTcpAccept(net, sock, fd, &rt->inAddrCli.sin_addr, ntohs(rt->inAddrCli.sin_port))) {
					closesocket(fd);
				} else {
				}
				if (++cnt >= 50) {
					break;
				}
			}
		} else if (sock->type == SOCKCTX_TYPE_TCP_CONN_WAIT) {
			if(FD_ISSET(sock->fd, sendSet)){  // conn succ
				net->cbTcpConnWait(net, sock, 0);
			}else if(FD_ISSET(sock->fd, exceptSet)){   //  conn failed
				int soErr = -1;
				socklen_t soErrLen = sizeof(soErr);
				getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, (char*)&soErr, &soErrLen);
				net->cbTcpConnWait(net, sock, trans_conn_err(soErr));
			}
		}
		if (--sockNumCnt < 1) {
			break;
		}
	}
	return 1;
}
int sock_poll_add_fd(SOCK_POLL_FD* p, struct socket_sockctx* sock, uint32_t events)
{
	int idx;
	if(!plk_pop(p->plkFreeIdx, &idx)){   // full
		if (!plk_pop(p->plkFreeIdxOri, &idx)) {
			return 0;
		}
	}
	assert(p->arrSockCtx[idx] == nullptr);
	p->arrSockCtx[idx] = sock;
	++p->sockNum;
	return 1;
}
int sock_poll_mod_fd(SOCK_POLL_FD* p, struct socket_sockctx* sock, uint32_t events)
{
	return 1;
}
int sock_poll_del_fd(SOCK_POLL_FD* p, struct socket_sockctx* sock)
{
	int& sockNum = p->sockNum;
	for (int i = 0; i < sockNum; ++i) {
		struct socket_sockctx* tmp = p->arrSockCtx[i];
		if(tmp && tmp->fd == sock->fd){
			p->arrSockCtx[i] = nullptr;
			--sockNum;
			plk_push(p->plkFreeIdx, i);
			return 1;
		}
	}
	return 0;
}
int sock_poll_init_tcpfd(struct socket_sockctx* sock, int sendBufLen, int recvBufLen)
{
	set_non_blocking(sock->fd);
	set_tcp_nodelay(sock->fd);
	set_keepalive(sock->fd);
	set_tcp_buf(sock->fd, sendBufLen, recvBufLen);
	return 1;
}

bool sock_tcp_listen(struct tcp_server_cfg* cfg, struct socket_sockctx* socks, int sockSize, int* pErr, int* errIdx)
{
	assert(sockSize >= cfg->addrNum);
	int err = -1;
	SOCK_FD addrFd[SOCK_TCP_LISTEN_ADDR_MAX];
	for (int i = 0; i < SOCK_TCP_LISTEN_ADDR_MAX; ++i) {
		addrFd[i] = INVALID_SOCKET;
	}
	do
	{
		bool hasErr = false;
		for (int i = 0; i<cfg->addrNum; ++i) {
			SOCKET fd = -1;
			sockaddr_in addrBind;
			memset(&addrBind, 0, sizeof(addrBind));
			addrBind.sin_family = AF_INET;
			addrBind.sin_addr.S_un.S_addr = cfg->addrs[i].S_un.S_addr;
			addrBind.sin_port = htons(cfg->port);
			fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (fd == INVALID_SOCKET)
			{//printf("socket failed: %s:%d, %d\n", cfg->host, cfg->port, errno);
				hasErr = true;
				*errIdx = i;
				err = SOCK_ERR_CREATE_FAILED;
				break;
			}
			// bind
			if (bind(fd, (struct sockaddr*)&addrBind, sizeof(addrBind)) == SOCKET_ERROR)
			{//printf("bind failed: %s:%d, %d\n", cfg->host, cfg->port, errno);
				hasErr = true;
				*errIdx = i;
				err = SOCK_ERR_BIND_FAILED;
				break;
			}
			// listen
			if (listen(fd, cfg->backlog) == SOCKET_ERROR)
			{//printf("listen failed: %s:%d, %d\n", cfg->host, cfg->port, errno);
				closesocket(fd);
				hasErr = true;
				*errIdx = i;
				err = SOCK_ERR_LISTEN_FAILED;
				break;
			}
			set_non_blocking(fd);
			addrFd[i] = fd;
		}
		if (hasErr) {
			break;
		}
		err = 0;
	} while (0);
	if (err == 0) {  // succ
		for (int i = 0; i < cfg->addrNum; ++i) {
			socks[i].fd = addrFd[i];
		}
		return true;
	}
	// close all fd has succ 
	for (int i = 0; i < cfg->addrNum; ++i) {
		if (addrFd[i] != INVALID_SOCKET) {
			closesocket(addrFd[i]);
		}
	}
	*pErr = err;
	return false;
}

static inline int trans_conn_err(int err)
{
	int ret = SOCK_ERR_CONN_FAILED;
	if (err == WSAECONNREFUSED) {
		ret = SOCK_ERR_CONN_REFUSED;
	} 
	else if (err == WSAENETUNREACH) {
		ret = SOCK_ERR_CONN_NETUNREACH;
	} 
	/*
	else if (err == ETIMEDOUT) {
		ret = SOCK_ERR_TIMEOUT;
	}
	*/
	return ret;
}
int sock_tcp_connect_nonblock(struct tcp_connect_cfg* cfg, SOCK_FD* pFd, SOCK_ADDR* pAddrRemote, int* inProgress)
{
	char port[16];
	sprintf_s(port, 16, "%d", cfg->port);
	struct addrinfo* aiList = nullptr;
	struct addrinfo aiHints;
	memset(&aiHints, 0, sizeof(aiHints));
	aiHints.ai_family = AF_INET;  // AF_INET(IPv4), AF_INET6(IPv6), AF_UNSPEC(IPv4 and IPv6)
	aiHints.ai_socktype = SOCK_STREAM;
	aiHints.ai_protocol = IPPROTO_TCP;
	//
	int ret = SOCK_ERR_CONN_FAILED;
	ret = getaddrinfo(cfg->host, port, &aiHints, &aiList);
	if (ret == 0) {  // getaddrinfo succ
		SOCKET fd = INVALID_SOCKET;
		ret = SOCK_ERR_CONN_FAILED;
		struct addrinfo *aiPtr = nullptr;
		for (aiPtr = aiList; aiPtr != nullptr; aiPtr = aiPtr->ai_next) {
			fd = socket(aiPtr->ai_family, aiPtr->ai_socktype, aiPtr->ai_protocol);
			if (fd == INVALID_SOCKET) {
				continue;
			}
			set_keepalive(fd);
			set_non_blocking(fd);
			set_tcp_nodelay(fd);
			set_tcp_buf(fd, cfg->sendBuf, cfg->recvBuf);
			//
			ret = connect(fd, aiPtr->ai_addr, aiPtr->ai_addrlen);
			if (ret != 0) {   // conn failed
				int err = WSAGetLastError();
				if(err != WSAEWOULDBLOCK){
					ret = trans_conn_err(err);
					closesocket(fd);
					fd = INVALID_SOCKET;
					continue;
				}
			}
			*pAddrRemote = ((struct sockaddr_in*)aiPtr->ai_addr)->sin_addr;
			if (ret != 0) {   // conn inprogress
				*inProgress = 1;
			} else {   // conn succ
				*inProgress = 0;
			}
			break;
		}
		if (fd != INVALID_SOCKET) {   // conn succ or inprogress
			*pFd = fd;
			ret = 0;
		}
	} else {
		ret = SOCK_ERR_ADDR_INVALID;
	}
	if (aiList) {
		freeaddrinfo(aiList);
	}
	return ret;
}

//
static void th_sleep(int milli)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(milli));
}
struct eventfd_init_ctx
{
	std::atomic<bool> succ;
	int port;
	SOCKET fdSend;
};
static void eventfd_thread(struct eventfd_init_ctx* ctx);
static int socket_listen(SOCKET* pSock, const char* host, int port, int backlog, int* err);
int sock_eventfd_new(SOCK_POLL_FD* pollFd, SOCK_EVENT_FD** ptr)
{
	WSAData wsa;
	int ret = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (ret != 0) {
		printf("WSAStartup failed: %d\n", ret);
		WSACleanup();
		return 0;
	}
	bool succ = false;
	SOCKET sockListen;
	int err = -1;
	int port;
	for (port = 50000; port < 50000 + 100; ++port) {
		if(socket_listen(&sockListen, "127.0.0.1", port, 16, &err)){
			succ = true;
			break;
		}else{  // listen error
			printf("eventfd listen at port %d failed: %d\n", port, err);
		}
	}
	if(!succ){   // listen failed
		printf("eventfd listen failed: %d\n", err);
		WSACleanup();
		return 0;
	}
	printf("eventfd listen succ at port %d\n", port);
	struct eventfd_init_ctx initCtx;
	initCtx.port = port;
	initCtx.succ.store(false);
	// start conn thread
	std::thread th = std::thread(eventfd_thread, &initCtx);
	struct sockaddr_in addrCli;
	int cliLen = 16;
	SOCKET sockEventRead;
	sockEventRead = accept(sockListen, (struct sockaddr*)&addrCli, &cliLen);
	th.join();
	if (sockEventRead == INVALID_SOCKET) {
		err = WSAGetLastError();
		printf("eventfd accept error: %d\n", err);
		closesocket(sockEventRead);
		closesocket(sockListen);
		WSACleanup();
		return 0;
	}
	if(!initCtx.succ.load()){
		printf("eventfd conn failed\n");
		closesocket(sockListen);
		WSACleanup();
		return 0;
	}
	// close init listen
	closesocket(sockListen);
	//
	printf("eventfd init all done, read=%d, send=%d\n", sockEventRead, initCtx.fdSend);
	SOCKET* arr = new SOCKET[2];
	arr[0] = sockEventRead;
	arr[1] = initCtx.fdSend;
	*ptr = arr;
	return 1;
}
int sock_eventfd_destroy(SOCK_EVENT_FD* ptr)
{
	closesocket(ptr[0]);
	closesocket(ptr[1]);
	delete[] ptr;
	return 1;
}

int sock_event_notify(SOCK_EVENT_FD* ptr)
{
	SOCKET fd = ptr[1];
	uint64_t step = 1;
	char ch;
	if (send(fd, &ch, 1, 0) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
		while (send(fd, &ch, 1, 0) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
			int n = 1;
		}
	}
	return 1;
}

int sock_pollfd_new(SOCK_POLL_FD** ptr)
{
	struct poll_ctx* p = new struct poll_ctx;
	p->plkFreeIdx = const_cast<struct pool_linked<int>*>(plk_create<int>(WIN_SOCK_MAX));
	p->plkFreeIdxOri = const_cast<struct pool_linked<int>*>(plk_create<int>(WIN_SOCK_MAX));
	for (int i = 0; i < WIN_SOCK_MAX; ++i) {
		p->arrSockCtx[i] = nullptr;
		plk_push(p->plkFreeIdxOri, i);
	}
	p->sockNum = 0;
	//
	*ptr = p;
	return 1;
}
int sock_pollfd_destroy(SOCK_POLL_FD* ptr)
{
	delete ptr;
	return 1;
}
//
static int set_non_blocking(SOCKET fd)
{
	unsigned long ul = 1;
	if(ioctlsocket(fd, FIONBIO, &ul) == SOCKET_ERROR){
		return 0;
	}
	return 1;
}
static void set_keepalive(SOCKET fd) 
{	
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char*)&on, sizeof(on));
}
static void set_tcp_nodelay(SOCKET fd)
{
	int on = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&on, sizeof(on));
}
static void set_tcp_buf(SOCKET fd, int sndLen, int rcvLen)
{
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&sndLen, sizeof(sndLen));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&rcvLen, sizeof(rcvLen));
}

//
void sock_fd_close(SOCK_FD* pFd)
{	
	SOCKET fd = *pFd;
	closesocket(fd);
}
void sock_fd_invalid(SOCK_FD* pFd)
{
	*pFd = INVALID_SOCKET;
}

int sock_addr_pton(SOCK_ADDR* addr, const char* strAddr)
{	
	if (inet_pton(AF_INET, strAddr, &addr->S_un.S_addr) == 1) {
		return 1;
	}
	return 0;
}
int sock_addr_ntop(SOCK_ADDR* addr, char* buf, size_t szBuf)
{
	assert(szBuf >= SOCK_ADDR_STRLEN);
	if (inet_ntop(AF_INET, &addr->S_un.S_addr, buf, SOCK_ADDR_STRLEN)) {
		return 1;
	}
	return 0;
}
int sock_addr_isinany(SOCK_ADDR* addr)
{
	return addr->S_un.S_addr == INADDR_ANY;
}
int sock_addr_equal(SOCK_ADDR* a1, SOCK_ADDR* a2)
{
	return a1->S_un.S_addr == a2->S_un.S_addr;
}

int sock_tcp_read(SOCK_FD fd, char* buf, int32_t size)
{
	int ret = recv(fd, buf, size, 0);
	if (ret > 0) {   // has read
		return ret;
	}
	if (ret == SOCKET_ERROR && (WSAGetLastError() == WSAEWOULDBLOCK)) {
		return READ_RET_AGAIN;
	}
	if (ret == 0) {
		return READ_RET_HALF_CLOSE;
	}
	return READ_RET_CLOSE;
}
int sock_tcp_send(SOCK_FD fd, const char* buf, int32_t size)
{
	int ret = send(fd, buf, size, 0);
	if (ret > 0) {
		return ret;
	}
	if (ret == SOCKET_ERROR && (WSAGetLastError() == WSAEWOULDBLOCK)) {
		return SEND_RET_AGAIN;
	}
	return SEND_RET_CLOSE;
}


int sock_poll_runtime_init(struct ST_POLL_RUNTIME** ptr)
{
	struct ST_POLL_RUNTIME* rt = new struct ST_POLL_RUNTIME;
	rt->addrLenCli = sizeof(rt->inAddrCli);
	*ptr = rt;
	return 1;
}
int sock_poll_runtime_deinit(struct ST_POLL_RUNTIME* ptr)
{
	delete ptr;
	return 1;
}

static int socket_listen(SOCKET* pSock, const char* host, int port, int backlog, int* err)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		*err = WSAGetLastError();
		return 0;
	}
	// bind
	sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, host, &addr.sin_addr.S_un.S_addr); //inet_addr(host);
	if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		*err = WSAGetLastError();
		closesocket(s);
		return 0;
	}
	// listen
	if (listen(s, backlog) == SOCKET_ERROR) {
		*err = WSAGetLastError();
		closesocket(s);
		return 0;
	}
	*pSock = s;
	return 1;
}
static void eventfd_thread(struct eventfd_init_ctx* ctx)
{
	th_sleep(1000);
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		int err = WSAGetLastError();
		printf("eventfd init thread failed: %d\n", err);
		return;
	}
	//
	int port = ctx->port;
	sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr.S_un.S_addr); //inet_addr(host);
	if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		int err = WSAGetLastError();
		printf("eventfd init thread conn failed: %d\n", err);
		return;
	}
	set_non_blocking(s);
	set_tcp_nodelay(s);
	ctx->fdSend = s;
	ctx->succ.store(true);
}

#endif