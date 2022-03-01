#include "pps_socket_epoll.h"


#if defined(PLAT_IS_LINUX)
#include "pps_net.h"
#include "pps_socket_define.h"

#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cerrno>


static int set_non_blocking(int fd);
static void set_keepalive(int fd);
static void set_tcp_nodelay(int fd);
static void set_tcp_buf(int fd, int sendLen, int recvLen);
static int get_sock_error(int fd);
int sock_poll(struct pps_net* net, int delay)
{
	struct ST_POLL_RUNTIME* rt = net->pollRuntime;
	struct epoll_event* evts = rt->events;
	int num;
	//printf("sock_poll wait, delay=%d\n", delay); // debug
	if (delay < 0) {  // block indefinitely
		num = epoll_wait(*net->pollFd, evts, EPOLL_EVENTS_MAX, -1);
	}
	else {
		num = epoll_wait(*net->pollFd, evts, EPOLL_EVENTS_MAX, delay);
	}
	//printf("sock_poll wakeup, evtNum=%d\n", num);   // debug
	if (num < 1) {
		return 0;
	}
	struct epoll_event* evt;
	struct socket_sockctx* sockCtx;
	struct socket_ctx* mainCtx;
	//
	for (int i=0; i<num; ++i) {  // it events
		evt = &evts[i];
		sockCtx = (struct socket_sockctx*)evt->data.ptr;
		printf("epollEvt: %d\n", evt->events);   // debug
		if (sockCtx->type == SOCKCTX_TYPE_TCP_CHANNEL) {
			if((evt->events & EPOLLIN) || (evt->events & EPOLLRDHUP) 
					|| (evt->events & EPOLLHUP) || (evt->events & EPOLLERR)) {
				net->cbTcpRead(net, sockCtx);
			}
			if (evt->events & EPOLLOUT) {
				net->cbTcpSend(net, sockCtx);
			}
		} else if (sockCtx->type == SOCKCTX_TYPE_EVENT) {
			uint64_t tmp64;
			read(*net->eventFd, &tmp64, 8); 
		} else if (sockCtx->type == SOCKCTX_TYPE_TCP_LISTEN) {
			int cnt = 0;
			int fd;
			while ((fd = accept(sockCtx->fd, (struct sockaddr*)&rt->inAddrCli, &rt->addrLenCli)) > -1) {
				if (!net->cbTcpAccept(net, sockCtx, fd, &rt->inAddrCli.sin_addr, ntohs(rt->inAddrCli.sin_port))) {
					close(fd);
				} else {
					//set_non_blocking(fd);
					//set_tcp_nodelay(fd);
					//set_keepalive(fd);
				}
				if (++cnt >= 50) {
					break;
				}
			}
		}else if(sockCtx->type == SOCKCTX_TYPE_TCP_CONN_WAIT){
			if(evt->events & EPOLLOUT){
				int errCode = get_sock_error(sockCtx->fd);
				net->cbTcpConnWait(net, sockCtx, errCode);
			}
		}
		
	}
	return 1;
}

int sock_poll_add_fd(SOCK_POLL_FD* pollFd, struct socket_sockctx* sock, uint32_t events)
{
	uint32_t event2 = 0;
	if (events&SOCK_POLLEVT_IN) {
		event2 = event2 | EPOLLIN;
	}
	if (events&SOCK_POLLEVT_OUT) {
		event2 = event2 | EPOLLOUT;
	}
	event2 = event2 | EPOLLRDHUP;
	//
	struct epoll_event evt;
	evt.data.ptr = sock;
	evt.events = event2;
	int ret = epoll_ctl(*pollFd, EPOLL_CTL_ADD, sock->fd, &evt);
	return ret;
}
int sock_poll_mod_fd(SOCK_POLL_FD* pollFd, struct socket_sockctx* sock, uint32_t events)
{
	uint32_t event2 = 0;
	if (events&SOCK_POLLEVT_IN) {
		event2 = event2 | EPOLLIN;
	}
	if (events&SOCK_POLLEVT_OUT) {
		event2 = event2 | EPOLLOUT;
	}
	event2 = event2 | EPOLLRDHUP;
	//
	struct epoll_event evt;
	evt.data.ptr = sock;
	evt.events = event2;
	int ret = epoll_ctl(*pollFd, EPOLL_CTL_MOD, sock->fd, &evt);
	return ret;
}
int sock_poll_del_fd(SOCK_POLL_FD* pollFd, struct socket_sockctx* sock)
{
	struct epoll_event evt;
	int ret = epoll_ctl(*pollFd, EPOLL_CTL_DEL, sock->fd, &evt);	
	return ret;
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
		addrFd[i] = -1;
	}
	do
	{	
		bool hasErr = false;
		for (int i=0; i<cfg->addrNum; ++i) {
			int fd = -1;
			struct sockaddr_in addrBind;
			memset(&addrBind, 0, sizeof(addrBind));
			addrBind.sin_family = AF_INET;
			addrBind.sin_addr = cfg->addrs[i];
			/*
			if (inet_pton(AF_INET, cfg->host, &addrBind.sin_addr) != 1)
			{
				//printf("parse tcp addr failed: %s:%d, %d\n", cfg->host, cfg->port, errno);
				err = SOCK_ERR_ADDR_INVALID;
				break;
			}
			*/
			addrBind.sin_port = htons(cfg->port);
			fd = socket(PF_INET, SOCK_STREAM, 0);
			if (fd < 0)
			{//printf("socket failed: %s:%d, %d\n", cfg->host, cfg->port, errno);
				hasErr = true;
				*errIdx = i;
				err = SOCK_ERR_CREATE_FAILED;
				break;
			}
			// bind
			if(bind(fd, (struct sockaddr*)&addrBind, sizeof(addrBind)) == -1)
			{//printf("bind failed: %s:%d, %d\n", cfg->host, cfg->port, errno);
				hasErr = true;
				*errIdx = i;
				err = SOCK_ERR_BIND_FAILED;
				break;
			}
			// listen
			if(listen(fd, cfg->backlog) == -1)
			{//printf("listen failed: %s:%d, %d\n", cfg->host, cfg->port, errno);
				close(fd);
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
		for(int i = 0 ; i < cfg->addrNum ; ++i) {
			socks[i].fd = addrFd[i];
		}
		return true;
	}
	// close all fd has succ 
	for(int i = 0 ; i < cfg->addrNum; ++i) {
		if (addrFd[i] > -1) {
			close(addrFd[i]);
		}
	}
	*pErr = err;
	return false;	
}

static inline int trans_conn_err(int err)
{
	int ret = SOCK_ERR_CONN_FAILED;
	if(err == ECONNREFUSED){
		ret = SOCK_ERR_CONN_REFUSED;
	}else if(err == ENETUNREACH){
		ret = SOCK_ERR_CONN_NETUNREACH;
	}else if(err == ETIMEDOUT){
		ret = SOCK_ERR_TIMEOUT;
	}
	return ret;
}
int sock_tcp_connect_nonblock(struct tcp_connect_cfg* cfg, SOCK_FD* pFd, SOCK_ADDR* pAddrRemote, int* inProgress)
{
	char port[16];
	sprintf(port, "%d", cfg->port);
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
		int fd = -1;
		ret = SOCK_ERR_CONN_FAILED;
		struct addrinfo *aiPtr = nullptr;
		for (aiPtr = aiList; aiPtr != nullptr; aiPtr = aiPtr->ai_next) {
			fd = socket(aiPtr->ai_family, aiPtr->ai_socktype, aiPtr->ai_protocol);
			if (fd < 0) {
				continue;
			}
			set_keepalive(fd);
			set_non_blocking(fd);
			set_tcp_nodelay(fd);
			set_tcp_buf(fd, cfg->sendBuf, cfg->recvBuf);
			//
			ret = connect(fd, aiPtr->ai_addr, aiPtr->ai_addrlen);
			if (ret != 0 && errno != EINPROGRESS) {   // conn failed
				ret = trans_conn_err(errno);
				close(fd);
				fd = -1;
				continue;
			}
			*pAddrRemote = ((struct sockaddr_in*)aiPtr->ai_addr)->sin_addr;
			if(ret != 0){   // conn inprogress
				*inProgress = 1;
			}else{   // conn succ
				*inProgress = 0;
			}
			break;
		}
		if(fd > -1){   // conn succ or inprogress
			*pFd = fd;
			ret = 0;
		}
	}else{
		ret = SOCK_ERR_ADDR_INVALID;
	}
	if (aiList) {
		freeaddrinfo(aiList);
	}
	return ret;
}
/*
			if(ret != 0 && errno == EINPROGRESS){   // in progress
				struct pollfd pollFd;
				pollFd.fd = fd;
				pollFd.events = POLLOUT;
				int timeout = cfg->timeout;
				if(timeout < 1){
					timeout = -1;  // infinite wait
				}
				int pollRet = poll(&pollFd, 1, timeout);
				if(pollRet < 1 || !(pollFd.events&POLLOUT)){   // timeout || poll error
					if(pollRet == 0){
						ret = SOCK_ERR_TIMEOUT;
					}else{
						ret = SOCK_ERR_CONN_FAILED;
					}
					close(fd);
					fd = -1;
				}else {   // writeable, check conn
					int soErr = -1;
					socklen_t soErrLen = sizeof(soErr);
					getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soErrLen);
					if(soErr == 0){   // conn succ
						*pAddrRemote = ((struct sockaddr_in*)aiPtr->ai_addr)->sin_addr;
					}else{   // conn failed
						ret = trans_conn_err(soErr);
						close(fd);
						fd = -1;	
					}
				}
			}*/
int get_sock_error(SOCK_FD fd)
{
	int soErr = -1;
	socklen_t soErrLen = sizeof(soErr);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soErrLen);
	if (soErr == 0) {   // conn succ
		return 0;
	} else {   // conn failed
		return trans_conn_err(soErr);
	}
}

//
int sock_eventfd_new(SOCK_POLL_FD* pollFd, SOCK_EVENT_FD** ptr)
{
	int* fd = new int;
	*fd = eventfd(0, EFD_NONBLOCK|EFD_SEMAPHORE);
	if (*fd < 0) {   // failed
		delete fd;
		return 0;
	}
	*ptr = fd;
	return 1;
}
int sock_eventfd_destroy(SOCK_EVENT_FD* ptr)
{
	int fd = *ptr;
	if (fd > -1) {
		close(fd);
	}
	return 1;
}

int sock_event_notify(SOCK_EVENT_FD* ptr)
{
	int fd = *ptr;
	uint64_t step = 1;
	if (write(fd, &step, 8) == -1 && errno == EAGAIN) {
		while (write(fd, &step, 8) == -1 && errno == EAGAIN) {
		
		}
	}
	return 1;
}
	
int sock_pollfd_new(SOCK_POLL_FD** ptr)
{
	int* fd = new int;
	*fd = epoll_create(EPOLL_EVENTS_MAX);
	if (*fd < 0) {   // failed
		delete fd;
		return 0;
	}
	*ptr = fd;
	return 1;	
}
int sock_pollfd_destroy(SOCK_POLL_FD* ptr)
{
	return 1;	
}
int sock_poll_runtime_init(struct ST_POLL_RUNTIME** ptr)
{
	struct ST_POLL_RUNTIME* st = new struct ST_POLL_RUNTIME;
	st->addrLenCli = sizeof(st->inAddrCli);
	*ptr = st;
	return 1;
}
int sock_poll_runtime_deinit(struct ST_POLL_RUNTIME* ptr)
{
	delete ptr;
	return 1;	
}
//
static int set_non_blocking(int fd)
{
	int oldOpt = fcntl(fd, F_GETFL);
	int newOpt = oldOpt | O_NONBLOCK;
	fcntl(fd, F_SETFL, newOpt);
	return oldOpt;
}
static void set_keepalive(int fd) {
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));  
}
static void set_tcp_nodelay(int fd)
{
	int on = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}
static void set_tcp_buf(int fd, int sndLen, int rcvLen)
{
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndLen, sizeof(sndLen));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvLen, sizeof(rcvLen));
}

//
void sock_fd_close(SOCK_FD* pFd)
{
	int fd = *pFd;
	if (fd > -1) {
		close(fd);	
	}
}
void sock_fd_invalid(SOCK_FD* pFd)
{
	*pFd = -1;
}

int sock_addr_pton(SOCK_ADDR* addr, const char* strAddr)
{
	if (inet_pton(AF_INET, strAddr, addr) == 1) {
		return 1;
	}
	return 0;
}
int sock_addr_ntop(SOCK_ADDR* addr, char* buf, size_t szBuf)
{
	assert(szBuf >= SOCK_ADDR_STRLEN);
	if (inet_ntop(AF_INET, addr, buf, SOCK_ADDR_STRLEN)) {
		return 1;
	}
	return 0;
}
int sock_addr_isinany(SOCK_ADDR* addr)
{
	return addr->s_addr == INADDR_ANY;	
}
int sock_addr_equal(SOCK_ADDR* a1, SOCK_ADDR* a2)
{
	return a1->s_addr == a2->s_addr;
}

int sock_tcp_read(SOCK_FD fd, char* buf, int32_t size)
{
	int ret = recv(fd, buf, size, 0);
	if (ret > 0) {   // has read
		return ret;
	}
	if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
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
	if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return SEND_RET_AGAIN;
	}
	return SEND_RET_CLOSE;
}

#endif


