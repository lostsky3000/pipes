#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <cstdint>
#include <atomic>
#include <array>
#include <mutex>
#include <condition_variable>
#include "mq_mpmc.h"

// debug sec begin
#include <list>
// debug sec end

//
//#define THREAD_POOL_DEBUG
//


#define THPOOL_MAX_THREAD 8
#define THPOOL_MAX_TASK 8

typedef bool(*THPOOL_CB_TICK)(void* ud);
typedef void(*THPOOL_CB_FREE)(void* ud);

struct thread_pool;

struct thread_pool_cfg
{
	uint32_t task_max;
	uint32_t thread_max;
	uint32_t idle_min;
	uint32_t idle_max;
};

struct thread_task 
{
	std::atomic<uint32_t> index;
	void* ud;
	THPOOL_CB_TICK cb_tick;
	THPOOL_CB_FREE cb_free;
};
struct thread_ctx
{
	std::atomic<uint32_t> index;
	struct thread_pool* pool;
	//
	std::mutex mtx;
	std::condition_variable cond;
};

struct thread_pool
{
	std::atomic<bool> is_shutdown;
	std::atomic<bool> shutdown_done;
	std::atomic<int32_t> task_num;
	std::atomic<int32_t> used_thread_num;
	std::atomic<int32_t> idle_thread_num;
	struct thread_pool_cfg cfg;
	mq_mpmc<uint32_t>* queue_task;
	mq_mpmc<uint32_t>* free_tasks;
	mq_mpmc<uint32_t>* free_ctxs;
	mq_mpmc<uint32_t>* idle_ctxs;
	std::array<struct thread_ctx, THPOOL_MAX_THREAD> arrCtx;
	std::array<struct thread_task, THPOOL_MAX_TASK> arrTask;
#ifdef THREAD_POOL_DEBUG
	std::atomic<int32_t> dbg_task_cnt;			  
#endif // THREAD_POOL_DEBUG
	
	// debug sec begin
	std::mutex dbgMtx;
	std::list<uint32_t> dbgLsTask;
	// debug sec end
	
	//
	thread_pool()
		: idle_thread_num(0)
		, used_thread_num(0)
		, task_num(0)
		, is_shutdown(false)
		, shutdown_done(false)
#ifdef THREAD_POOL_DEBUG
	    , dbg_task_cnt(0)			  
#endif // THREAD_POOL_DEBUG
	{}
};

//
int thpool_post_task(const struct thread_pool* pool, void*ud, 
		THPOOL_CB_TICK cbTick, THPOOL_CB_FREE cbFree, int32_t* err=nullptr);

int thpool_shutdown_sync(const struct thread_pool* pool);

const struct thread_pool* thpool_create(struct thread_pool_cfg* cfg);
void thpool_destroy(const struct thread_pool* pool);

int thpool_is_shutdown(const struct thread_pool* pool);

uint32_t thpool_thread_limit();
uint32_t thpool_task_limit();

#endif // !THREAD_POOL_H

