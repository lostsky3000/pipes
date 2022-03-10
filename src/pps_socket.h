#ifndef PPS_SOCKET_H
#define PPS_SOCKET_H


#include <cstdint>
#include <atomic>
#include <list>
#include <cstring>
#include "mq_mpmc.h"
#include "pps_net_task.h"
#include "pool_linked.h"
#include "pps_malloc.h"
#include "pps_socket_plat.h"


#define SOCK_ADDR_STRLEN 16
//
#define SOCK_SEND_BUF_MIN 1024
#define SOCK_SEND_BUF_MAX 4096
#define SOCK_RECV_BUF_MIN 1024
#define SOCK_RECV_BUF_MAX 8192
#define SOCK_BACKLOG_DEF 128
#define SOCK_BACKLOG_MAX 8192
#define SOCK_CONN_TIMEOUT_MAX 1000*30

//
#define SOCKCTX_TYPE_EVENT 1
#define SOCKCTX_TYPE_TCP_LISTEN 2
#define SOCKCTX_TYPE_TCP_CHANNEL 3
#define SOCKCTX_TYPE_TCP_CONN_WAIT 4
#define SOCKCTX_TYPE_UDP 5

//
#define SOCK_RT_BEGIN(c) sock_rt_begin(c)
#define SOCK_RT_END(c) sock_rt_end(c)


//
#define READ_RET_CLOSE -1
#define READ_RET_AGAIN -2
#define READ_RET_HALF_CLOSE -3

#define SEND_RET_CLOSE -1
#define SEND_RET_AGAIN -2

//
#define DECTYPE_NOW 0
#define DECTYPE_LEN 1
#define DECTYPE_SEP 2
#define TCP_DEC_NUM 3

typedef int(*CB_DEC_ON_READ)(int total, int cur, char* buf, int size, void*ud);

struct read_decode
{
	int type;
	int session;
};
struct read_decode_now
{
	struct read_decode head;
};
struct read_decode_len
{
	struct read_decode head;
	int readLen;
};

struct read_buf_block;
struct read_decode_sep
{
	struct read_decode head;
	int sepLen;
	int sepCap;
	int seekedBytes;
	int sepCharSeek;
	char* sep;
	struct read_buf_block* seekBuf;
	int seekBufPos;
};

struct dec_arg_len
{
	int readLen;
};
struct dec_arg_sep
{
	int sepLen;
	char* sep;
};
struct read_arg
{
	int16_t decType;
	int16_t isNewRead;
	int32_t srcIdx;
	uint32_t srcCnt;
	int32_t maxRead;
	int session;
	CB_DEC_ON_READ cb;
	void* ud;
	void* decArg;
};

struct read_buf_block
{
	uint32_t idx;
	int32_t cap;
	int32_t size4Fill;
	int32_t size4Read;
	char* buf;	
	struct read_buf_block* next;
};
struct read_buf_queue
{
	struct read_buf_block* head;
	struct read_buf_block* tail;
};
struct read_runtime
{
	bool onReadWait;
	bool sockClosed;
	bool hasSendReadOver;
	std::atomic<int8_t> readLock;
	int preLockOwner;
	uint32_t sockCnt;
	int32_t waitReadable;
	int readWaitDecType;
	std::atomic<int32_t> readable;
	std::atomic<uint32_t> fillBufIdx;
	uint32_t fillBufIdx4Net;
	std::atomic<uint32_t> readBufIdx;
	struct read_buf_block* curFillBuf;
	struct read_buf_block* curReadBuf;
	struct read_buf_queue queReading;
	struct read_buf_queue queFree;
	struct read_decode* curDec;
	struct read_decode* arrDecode[TCP_DEC_NUM];
	//
	int32_t readPackMax;
	CB_DEC_ON_READ cbRead;
	void* udRead;
};
inline void rdbuf_clear(struct read_buf_queue* q)
{
	q->head = nullptr;
	q->tail = nullptr;
}
inline void rdbuf_push(struct read_buf_queue* q, struct read_buf_block* buf)	
{
	buf->next = nullptr;
	if (q->tail) {
		q->tail->next = buf;
		q->tail = buf;
	}
	else {
		q->head = buf;
		q->tail = buf;
	}
}
inline struct read_buf_block* rdbuf_pop(struct read_buf_queue* q)	
{
	struct read_buf_block* buf = q->head;
	if (buf) {
		q->head = buf->next;
		buf->next = nullptr;
		if (q->head == nullptr) {
			q->tail = nullptr;
		}
	}
	return buf;
}
inline struct read_buf_block* rdbuf_front(struct read_buf_queue* q)
{
	return q->head;
}
inline void rdbuf_reset(struct read_buf_block* b, int32_t newCap, uint32_t idx)
{
	if (b->buf && b->cap < newCap) {
		pps_free(b->buf);
		b->buf = nullptr;
	}
	if (b->buf == nullptr) {
		b->cap = newCap;
		b->buf = (char*)pps_malloc(newCap);
	}
	b->idx = idx;
	b->next = nullptr;
	b->size4Fill = 0;
	b->size4Read = 0;
}
inline struct read_buf_block* rdbuf_new(int32_t cap, uint32_t idx)	
{
	struct read_buf_block* b = new struct read_buf_block;
	memset(b, 0, sizeof(struct read_buf_block));
	rdbuf_reset(b, cap, idx);
	return b;
}
inline void rdbuf_free(struct read_buf_block* b)	
{
	if (b->buf) {
		pps_free(b->buf);
		b->buf = nullptr;
	}
	delete b;
}

//
struct send_buf_block
{
	uint32_t idx;
	int cap;
	int size4Fill;
	int size4Send;
	char* buf;
	struct send_buf_block* next;
};
struct send_buf_queue
{
	struct send_buf_block* head;
	struct send_buf_block* tail;
};
struct send_runtime
{
	bool closeCalled4Svc;
	bool sockClosed;
	int16_t preLockOwner;
	uint32_t sockCnt;
	std::atomic<bool> sendLock;
	std::atomic<int32_t> unSendBytes;
	std::atomic<uint32_t> fillBufIdx;
	std::atomic<uint32_t> sendBufIdx;
	uint32_t fillBufIdxLocal;
	struct send_buf_block* curFillBuf;
	struct send_buf_block* curSendBuf;
	struct send_buf_queue queUnSend;
	struct send_buf_queue queFree;
};
inline void sdbuf_clear(struct send_buf_queue* q)
{
	q->head = nullptr;
	q->tail = nullptr;
}
inline void sdbuf_push(struct send_buf_queue* q, struct send_buf_block* buf)	
{
	buf->next = nullptr;
	if (q->tail) {
		q->tail->next = buf;
		q->tail = buf;
	}
	else {
		q->head = buf;
		q->tail = buf;
	}
}
inline struct send_buf_block* sdbuf_pop(struct send_buf_queue* q)	
{
	struct send_buf_block* buf = q->head;
	if (buf) {
		q->head = buf->next;
		buf->next = nullptr;
		if (q->head == nullptr) {
			q->tail = nullptr;
		}
	}
	return buf;
}
inline struct send_buf_block* sdbuf_front(struct send_buf_queue* q)
{
	return q->head;
}
inline void sdbuf_reset(struct send_buf_block* b, int32_t newCap, uint32_t idx)
{
	if (b->buf && b->cap < newCap) {
		pps_free(b->buf);
		b->buf = nullptr;
	}
	if (b->buf == nullptr) {
		b->cap = newCap;
		b->buf = (char*)pps_malloc(newCap);
	}
	b->idx = idx;
	b->next = nullptr;
	b->size4Fill = 0;
	//b->size4Read = 0;
}
inline struct send_buf_block* sdbuf_new(int32_t cap, uint32_t idx)	
{
	struct send_buf_block* b = new struct send_buf_block;
	memset(b, 0, sizeof(struct send_buf_block));
	sdbuf_reset(b, cap, idx);
	return b;
}

//
struct pipes;
struct pps_net;
struct socket_ctx;

struct socket_sockctx
{
	int16_t idx;
	int16_t type;
	SOCK_FD fd;
	struct socket_ctx* main;
};
struct socket_ctx
{
	bool pollInReg;
	bool pollOutReg;
	bool closeCalled4Net;
	bool readOver4Net;
	int16_t type;
	int16_t idxNet;
	//
	int32_t idx;
	std::atomic<uint32_t> cnt;
	uint32_t cnt4Net;
	//
	struct netreq_src src;
	//
	int32_t sendBufLen;
	int32_t recvBufLen;
	//
	SOCK_ADDR addrRemote;
	int portRemote;
	//
	int16_t sockCap;
	int16_t sockNum;
	struct socket_sockctx* socks;
	//
	void* ud;
	//
	struct read_runtime readRuntime;
	struct send_runtime sendRuntime;
};

struct socket_mgr
{
	uint32_t ctxPoolCap;
	std::atomic<uint32_t> ctxUsedNum;
	struct socket_ctx* ctxPool;
	struct pipes* pipes;
	struct mq_mpmc<uint32_t>* queCtxFree;
	struct mq_mpmc<uint32_t>* queCtxOri;
};

//
void sockmgr_init_main_thread(struct socket_mgr* mgr, struct pipes* pipes);
void sockmgr_deinit_main_thread(struct socket_mgr* mgr);

//
int32_t sock_slot_alloc(struct socket_mgr* mgr);
int sock_slot_free(struct socket_mgr* mgr, uint32_t idx);
inline struct socket_ctx* sock_get_ctx(struct socket_mgr* mgr, uint32_t idx)
{
	return &mgr->ctxPool[idx];	
}
inline struct socket_ctx* sock_valid_ctx(struct socket_mgr* mgr, int idx, uint32_t cnt)
{
	if (idx < 0 || idx >= SOCK_CTX_SLOT_NUM) {
		return nullptr;
	}
	struct socket_ctx* ctx = &mgr->ctxPool[idx];
	if (ctx) {
		if (cnt != ctx->cnt.load(std::memory_order_acquire)) {   // SOCK_RT_BEGIN
			return nullptr;
		}
	}
	return ctx;
}
inline void sock_rt_begin(struct socket_ctx* ctx) {
	ctx->cnt.load(std::memory_order_acquire);
}
inline void sock_rt_end(struct socket_ctx* ctx)
{
	ctx->cnt.fetch_add(0, std::memory_order_release);
}

//
inline void sock_read_initlock(struct socket_ctx* ctx) {
	ctx->readRuntime.readLock.store(0, std::memory_order_relaxed);
}
inline bool sock_read_trylock(struct socket_ctx* ctx)
{
	return ctx->readRuntime.readLock.exchange(1, std::memory_order_acquire) == 0;
}
inline void sock_read_unlock(struct socket_ctx* ctx)
{
	assert(ctx->readRuntime.readLock.exchange(0, std::memory_order_release) == 1);
}
inline void sock_read_atom_acquire(struct socket_ctx* ctx)
{
	ctx->readRuntime.readLock.load(std::memory_order_acquire);
}
inline void sock_read_atom_release(struct socket_ctx* ctx)
{
	ctx->readRuntime.readLock.fetch_add(0, std::memory_order_release);
}

//
inline void sock_send_initlock(struct socket_ctx* ctx)
{
	ctx->sendRuntime.sendLock.store(false, std::memory_order_relaxed);
}
inline int sock_send_trylock(struct socket_ctx* ctx) {
	return ctx->sendRuntime.sendLock.exchange(true, std::memory_order_acquire) == false;
}
inline void sock_send_unlock(struct socket_ctx* ctx)
{
	assert(ctx->sendRuntime.sendLock.exchange(false, std::memory_order_release));
}
inline void sock_send_atom_acquire(struct socket_ctx* ctx)
{
	ctx->sendRuntime.sendLock.load(std::memory_order_acquire);	
}
int sock_ctxsocks_init(struct socket_ctx* ctx, int sockCap);
inline int sock_ctxsocks_set(struct socket_ctx* ctx, int idx, SOCK_FD* pFd, int type)
{
	struct socket_sockctx* s = &ctx->socks[idx];
	s->fd = *pFd;
	s->type = (int16_t)type;
	return 1;
}

// plat relative
int sock_eventfd_new(SOCK_POLL_FD* pollFd, SOCK_EVENT_FD** ptr);
int sock_eventfd_destroy(SOCK_EVENT_FD* ptr);
int sock_event_notify(SOCK_EVENT_FD* ptr);
int sock_pollfd_new(SOCK_POLL_FD** ptr);
int sock_pollfd_destroy(SOCK_POLL_FD* ptr);

int sock_poll_runtime_init(struct ST_POLL_RUNTIME** ptr);
int sock_poll_runtime_deinit(struct ST_POLL_RUNTIME* ptr);

int sock_poll_add_fd(SOCK_POLL_FD* pollFd, struct socket_sockctx* sock, uint32_t events);
int sock_poll_mod_fd(SOCK_POLL_FD* pollFd, struct socket_sockctx* sock, uint32_t events);
int sock_poll_del_fd(SOCK_POLL_FD* pollFd, struct socket_sockctx* sock);
int sock_poll_init_tcpfd(struct socket_sockctx* sock, int sendBufLen, int recvBufLen);

void sock_fd_close(SOCK_FD* pFd);
void sock_fd_invalid(SOCK_FD* pFd);

int sock_addr_pton(SOCK_ADDR* addr, const char* strAddr);
int sock_addr_ntop(SOCK_ADDR* addr, char* buf, size_t szBuf);
int sock_addr_isinany(SOCK_ADDR* addr);
int sock_addr_equal(SOCK_ADDR* a1, SOCK_ADDR* a2);

int sock_tcp_read(SOCK_FD fd, char* buf, int32_t size);
int sock_tcp_send(SOCK_FD fd, const char* buf, int32_t size);

bool sock_tcp_listen(struct tcp_server_cfg* cfg, struct socket_sockctx* socks, int sockSize, int* pErr, int* errIdx);
int sock_tcp_connect_nonblock(struct tcp_connect_cfg* cfg, SOCK_FD* pFd, SOCK_ADDR* pAddrRemote, int* inProgress);
//int sock_tcp_check_connect_done(SOCK_FD* pFd);
//
int sock_poll(struct pps_net* net, int delay);


// func 4 readDec
typedef int(*FN_READDEC_INIT)(struct read_runtime* run, struct read_arg* arg);
int sockdec_init_now(struct read_runtime* run, struct read_arg* arg);
int sockdec_init_len(struct read_runtime* run, struct read_arg* arg);
int sockdec_init_sep(struct read_runtime* run, struct read_arg* arg);
typedef int(*FN_READDEC_READ)(struct read_runtime* run, int* notifyReadable, int*readableUsed, int* trunc);
int sockdec_read_sep(struct read_runtime* run, int* waitReadable, int*readableUsed, int* trunc);
int sockdec_read_len(struct read_runtime* run, int* waitReadable, int*readableUsed, int* trunc);
int sockdec_read_now(struct read_runtime* run, int* waitReadable, int*readableUsed, int* trunc);

bool sockdec_sep_seek(struct read_decode_sep* d, int readableBytes);

#endif // !PPS_SOCKET_H

