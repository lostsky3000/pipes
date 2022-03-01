#include "thread_pool.h"
#include <thread>

static int dbg_task_push(struct thread_pool* p, uint32_t idx)
{
	int ret = 1;
	std::unique_lock<std::mutex> lock(p->dbgMtx);
	p->dbgLsTask.push_back(idx);
	return ret;
}

static int dbg_task_pop(struct thread_pool* p, uint32_t idx) 
{
	int ret = 0;
	std::unique_lock<std::mutex> lock(p->dbgMtx);
	for (std::list<uint32_t>::iterator it = p->dbgLsTask.begin();
			it != p->dbgLsTask.end(); ++it) {
				if (*it == idx) {   // find
					p->dbgLsTask.erase(it);
					ret = 1;
					break;
				}
	}
	return ret;
}


static inline void back_ctx(struct thread_ctx* ctx)
{
	assert(mpmc_push(ctx->pool->free_ctxs, ctx->index.load(std::memory_order_relaxed)));
	// decr active thread num
	ctx->pool->used_thread_num.fetch_sub(1);
#ifdef THREAD_POOL_DEBUG
	printf("thread end, idx=%d\n", ctx->index.load(std::memory_order_relaxed));			  
#endif // THREAD_POOL_DEBUG
}

static void fn_thread(struct thread_ctx* ctx)
{
#ifdef THREAD_POOL_DEBUG
	printf("thread start, idx=%d\n", ctx->index.load(std::memory_order_relaxed));			  
#endif // THREAD_POOL_DEBUG

	struct thread_pool* p = ctx->pool;
	struct thread_pool_cfg* cfg = &p->cfg;
	uint32_t idxTask = 0;
	struct thread_task* task = nullptr;
	int32_t tempWaitCnt = 0;
	while (true) {
		if (p->is_shutdown.load()) {
			break;
		}
		while (mpmc_pop(p->queue_task, &idxTask)) {  // proc task loop
			tempWaitCnt = 0;
			task = &p->arrTask[idxTask];
			if (!task->cb_tick) {
				// debug
				int n = dbg_task_pop(p, idxTask);      // debug
				//continue;
				n = 1; 
			}
			else {
				int n = dbg_task_pop(p, idxTask);
				if (!n) {
					int m = 1;
				}
			}
			while (!task->cb_tick(task->ud)) {
		
			}
			task->cb_tick = nullptr;
			task->cb_free = nullptr;
			task->ud = nullptr;
#ifdef THREAD_POOL_DEBUG
			int32_t taskCnt = p->dbg_task_cnt.fetch_add(1) + 1;
			if (taskCnt % 100 == 0) {
				printf("============ taskCallCnt, now=%d, taskQLen=%d\n", 
					taskCnt, mpmc_size(p->queue_task));		
			}	  
#endif // THREAD_POOL_DEBUG
			//back task
			assert(mpmc_push(p->free_tasks, idxTask));
			p->task_num.fetch_sub(1);
			//
			if(p->is_shutdown.load()) {
				break;
			}
		}
		//
		if(p->task_num.load() <= 0) {  // no more task, check idle
			if(cfg->idle_min <= 0) {   // no need idle, quit
				break;
			}
			int32_t idleNum = p->idle_thread_num.fetch_add(1);
			if (idleNum >= cfg->idle_max) { // more than idle_max, quit
				p->idle_thread_num.fetch_sub(1);
				break;
			}
			if (p->is_shutdown.load()) {
				p->idle_thread_num.fetch_sub(1);
				break;
			}
			if (idleNum < cfg->idle_min) {   // < than idle_min, wait until notify
#ifdef THREAD_POOL_DEBUG
	printf("idle begin, idx=%d\n", ctx->index.load(std::memory_order_relaxed));			  
#endif // THREAD_POOL_DEBUG
				{
					std::unique_lock<std::mutex> lock(ctx->mtx);
					assert(mpmc_push(p->idle_ctxs, ctx->index.load(std::memory_order_relaxed)));
					ctx->cond.wait(lock);
				 }
#ifdef THREAD_POOL_DEBUG
				printf("idle end, idx=%d\n", ctx->index.load(std::memory_order_relaxed));			  
#endif // THREAD_POOL_DEBUG
			}
			p->idle_thread_num.fetch_sub(1);
		}
	}
	// back thread ctx
	back_ctx(ctx);
}

int thpool_post_task(const struct thread_pool* pool, void*ud,
	THPOOL_CB_TICK cbTick, THPOOL_CB_FREE cbFree, int32_t* pErr)
{
	struct thread_pool* p = const_cast<struct thread_pool*>(pool);
	if (p->is_shutdown.load()) {
		if (pErr) {
			*pErr = -1;
		}
		return 0;
	}
	if (p->task_num.fetch_add(1) >= p->cfg.task_max) {
		  // reach cfg->max_task
		p->task_num.fetch_sub(1);
		if (pErr) {
			*pErr = -2;
		}
		return 0;
	}
	uint32_t idx = 0;
	if (!mpmc_pop(p->free_tasks, &idx)) { // no more free task slot
		p->task_num.fetch_sub(1);
		if (pErr) {
			*pErr = -3;
		}
		return 0;
	}
	// add to task queue
	struct thread_task* task = &p->arrTask[idx];
	if (task->cb_tick || task->cb_free || task->ud) {  // debug
		int n = 1;
	}
	task->cb_tick = cbTick;
	task->cb_free = cbFree;
	task->ud = ud;
	//
	dbg_task_push(p, idx); // debug
	//
	assert(mpmc_push(p->queue_task, idx));
	// alloc thread
	idx = 0;
	if (mpmc_pop(p->idle_ctxs, &idx)) {
#ifdef THREAD_POOL_DEBUG
		printf("pop idle_ctxs, idx=%d\n", idx);			  
#endif // THREAD_POOL_DEBUG
		// has idle thread, notify
		struct thread_ctx* idleCtx = &p->arrCtx[idx];
		std::unique_lock<std::mutex> lock(idleCtx->mtx);
		idleCtx->cond.notify_one();
	}
	else {  // no idle thread, new
		if(p->used_thread_num.fetch_add(1) < p->cfg.thread_max) {
			  // can new thread
			uint32_t idxFree = 0;
			if (mpmc_pop(p->free_ctxs, &idxFree)) { // new thread
				struct thread_ctx* freeCtx = &p->arrCtx[idxFree];
				std::thread th = std::thread(fn_thread, freeCtx);
				th.detach();
			}
			else {  // no more free thread
				p->used_thread_num.fetch_sub(1);
			}
		}else {  // reach cfg->max_thread, no more new-thread
			p->used_thread_num.fetch_sub(1);
		}
	}
	return 1;
}

int thpool_shutdown_sync(const struct thread_pool* pool)
{
	struct thread_pool* p = const_cast<struct thread_pool*>(pool);
	if (!p->is_shutdown.exchange(true)) {  // first call shutdown
	
	}
	uint32_t idx = 0;
	while (true) {
		if(mpmc_pop(p->idle_ctxs, &idx)) {
			struct thread_ctx* ctx = &p->arrCtx[idx];
			std::unique_lock<std::mutex> lock(ctx->mtx);
			ctx->cond.notify_all();
		}else if(p->used_thread_num.load() <= 0 && p->idle_thread_num.load() <= 0) {
			break;
		}
	}
	p->shutdown_done.store(true);
	return 1;
}

const struct thread_pool* thpool_create(struct thread_pool_cfg* cfg)
{
	assert(cfg->task_max <= THPOOL_MAX_TASK);
	assert(cfg->thread_max <= THPOOL_MAX_THREAD);
	assert(cfg->idle_max >= cfg->idle_min);
	assert(cfg->idle_max <= cfg->thread_max);
	//
	struct thread_pool* pool = new struct thread_pool;
	pool->cfg = *cfg;
	pool->queue_task = const_cast<mq_mpmc<uint32_t>*>
		(mpmc_create<uint32_t>(THPOOL_MAX_TASK));
	pool->free_ctxs = const_cast<mq_mpmc<uint32_t>*>
		(mpmc_create<uint32_t>(THPOOL_MAX_THREAD));
	pool->idle_ctxs = const_cast<mq_mpmc<uint32_t>*>
		(mpmc_create<uint32_t>(THPOOL_MAX_THREAD));
	pool->free_tasks = const_cast<mq_mpmc<uint32_t>*>
		(mpmc_create<uint32_t>(THPOOL_MAX_TASK));
	for (uint32_t i=0; i<THPOOL_MAX_THREAD; ++i) {
		pool->arrCtx[i].index = i;
		pool->arrCtx[i].pool = pool;
		mpmc_push(pool->free_ctxs, i);
	}
	for (uint32_t i=0; i<THPOOL_MAX_TASK; ++i) {
		pool->arrTask[i].index = i;
		mpmc_push(pool->free_tasks, i);
	}
	return pool;
}

void thpool_destroy(const struct thread_pool* pool)
{
	struct thread_pool* p = const_cast<struct thread_pool*>(pool);
	// clear task-ud
	uint32_t idx = 0;
	while (mpmc_pop(p->queue_task, &idx)) {
		struct thread_task* t = &p->arrTask[idx];
		if (t->ud) {
			if (t->cb_free) {
				t->cb_free(t->ud);
			}
			t->cb_free = nullptr;
			t->ud = nullptr;
		}
	}
	//
	mpmc_destroy(p->queue_task);
	p->queue_task = nullptr;
	mpmc_destroy(p->free_tasks);
	p->free_tasks = nullptr;
	mpmc_destroy(p->free_ctxs);
	p->free_ctxs = nullptr;
	mpmc_destroy(p->idle_ctxs);
	p->idle_ctxs = nullptr;
	delete p;	
}

int thpool_is_shutdown(const struct thread_pool* pool)
{
	return pool->shutdown_done.load();
}

uint32_t thpool_thread_limit()
{
	return THPOOL_MAX_THREAD;
}

uint32_t thpool_task_limit()
{
	return THPOOL_MAX_TASK;
}

