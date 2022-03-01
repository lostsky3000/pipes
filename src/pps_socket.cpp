#include "pps_socket.h"

int32_t sock_slot_alloc(struct socket_mgr* mgr)
{
	if (mgr->ctxUsedNum.fetch_add(1, std::memory_order_relaxed) + 1 >= MAX_SOCK_CTX) {
		mgr->ctxUsedNum.fetch_sub(1, std::memory_order_relaxed);
		return -2;
	}
	uint32_t idx;
	if (mpmc_pop(mgr->queCtxFree, &idx)) {
		return idx;
	}
	if (mpmc_pop(mgr->queCtxOri, &idx)) {
		return idx;
	}
	// warning?
	mgr->ctxUsedNum.fetch_sub(1, std::memory_order_relaxed);
	return -1;
}
int sock_slot_free(struct socket_mgr* mgr, uint32_t idx)
{
	assert(mpmc_push(mgr->queCtxFree, idx));
	mgr->ctxUsedNum.fetch_sub(1, std::memory_order_relaxed);
	return 1;	
}

int sock_ctxsocks_init(struct socket_ctx* ctx, int sockCap)
{
	if (sockCap < 1) { // clear socks
		if(ctx->socks) {
			ctx->sockCap = 0;
			delete[] ctx->socks;
			ctx->socks = nullptr;
		}
		return 1;
	}
	if (ctx->socks && sockCap > ctx->sockCap) {
		ctx->sockCap = 0;
		delete[] ctx->socks;
		ctx->socks = nullptr;
	}
	if (ctx->socks == nullptr) {
		ctx->socks = new struct socket_sockctx[sockCap];
		ctx->sockCap = sockCap;
		for (int i = 0; i < sockCap; ++i) {
			ctx->socks[i].idx = i;
			ctx->socks[i].main = ctx;
		}
	}
	return 1;
}


//
void sockmgr_init_main_thread(struct socket_mgr* mgr, struct pipes* pipes)
{
	mgr->pipes = pipes;
	mgr->ctxPoolCap = SOCK_CTX_SLOT_NUM;
	mgr->ctxPool = new struct socket_ctx[mgr->ctxPoolCap];
	mgr->queCtxFree = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(mgr->ctxPoolCap));
	mgr->queCtxOri = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(mgr->ctxPoolCap));
	for (uint32_t i = 0; i < mgr->ctxPoolCap; ++i) {
		struct socket_ctx* ctx = &mgr->ctxPool[i];
		//SOCK_RT_BEGIN(ctx);
		ctx->socks = nullptr;
		ctx->sockCap = 0;
		ctx->sockNum = 0;
		sock_ctxsocks_init(ctx, 0);
		// ctx init stuff
		ctx->idx = i;
		ctx->cnt.store(0);
		SOCK_RT_END(ctx);
		//
		//SOCK_RDRT_BEGIN(ctx);
		sock_read_initlock(ctx);
		struct read_runtime* run = &ctx->readRuntime;
		rdbuf_clear(&run->queReading);
		rdbuf_clear(&run->queFree);
		run->curDec = nullptr;
		for (int i = 0; i < TCP_DEC_NUM; ++i) {
			run->arrDecode[i] = nullptr;
		}
		sock_read_atom_release(ctx);
		//
		struct send_runtime* srun = &ctx->sendRuntime;
		sock_send_initlock(ctx);
		assert(sock_send_trylock(ctx));
		sdbuf_clear(&srun->queUnSend);
		sdbuf_clear(&srun->queFree);
		sock_send_unlock(ctx);
		//
		mpmc_push(mgr->queCtxOri, i);
	}
	mgr->ctxUsedNum.store(0);
}

void sockmgr_deinit_main_thread(struct socket_mgr* mgr)
{
	for (uint32_t i = 0; i < mgr->ctxPoolCap; ++i) {
		struct socket_ctx* ctx = &mgr->ctxPool[i];
		// ctx deinit stuff
		SOCK_RT_BEGIN(ctx);
		sock_ctxsocks_init(ctx, 0);
		SOCK_RT_END(ctx);
	}
	delete[] mgr->ctxPool;
	//
	mpmc_destroy(mgr->queCtxFree);
	mpmc_destroy(mgr->queCtxOri);
}

