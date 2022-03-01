#ifndef PPS_TIMER_H
#define PPS_TIMER_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "mq_mpsc.h"
#include "mq_mpmc.h"
#include "pool_linked.h"
#include "timing_wheel.h"
#include "pps_macro.h"
#include "pipes.h"

#define TIMER_HIGH_PRECISION



////////
#if defined(TIMER_HIGH_PRECISION) && defined(PLAT_IS_LINUX)
#define USE_LINUX_TIMER
#endif 


#ifdef USE_LINUX_TIMER
#include <pthread.h>	
#include <sched.h>
struct timer_clock_point
{
	struct timespec tm;
};
struct timer_cond
{
	pthread_mutex_t mtx;
	pthread_condattr_t condAttr;
	pthread_cond_t cond;
};
#else
struct timer_clock_point
{
	std::chrono::time_point<std::chrono::steady_clock> tm;
};
struct timer_cond
{
	std::mutex mtx;
	std::condition_variable cv;
};
#endif


struct pipes;
struct timer_msg 
{
	uint32_t svcIdx;
	uint32_t svcCnt;
	int32_t delay;
	int32_t session;
	int32_t repeat;
	int64_t expiration;
};

struct pps_timer;
struct timer_task;

struct pps_timer
{
	std::atomic<bool> idle;
	uint32_t index;
	std::atomic<uint32_t> unAddTask;
	std::atomic<uint32_t> taskBakNum;
	uint32_t reAddTask;
	uint32_t unRspTask;
	struct pipes* pipes;
	//
	struct tw_timer<struct timer_task*>* twTimer;
	//
	struct pool_linked<struct timer_task*>* plkReAddTask;
	//
	struct pool_linked<struct timer_msg>* plkUnRspTask;
	//
	struct pool_linked<struct timer_task*>* plkTaskBak;
	//
	struct timer_task* taskSlots;
	mq_mpmc<int32_t>* freeTaskPool;
	mq_mpsc<int32_t>* queTask;
	//
	std::mutex mtxTaskBak;
	//
	struct timer_cond idleCond;
};

#if defined(USE_LINUX_TIMER)
	void* timer_thread(void* arg);
#else
	void timer_thread(struct pps_timer* tmr);
#endif


int timer_add_task(struct timer_msg*m, struct pps_timer* timer);

void timer_shutdown(struct pipes* pipes);

//
inline void timer_clock_now(struct timer_clock_point* tm)
{
#ifdef USE_LINUX_TIMER
	clock_gettime(CLOCK_MONOTONIC, &tm->tm);
#else
	using namespace std::chrono;
	tm->tm = steady_clock::now();
#endif
}
inline int64_t timer_clock_now_ms(struct pipes* pipes)
{
#ifdef USE_LINUX_TIMER
	struct timespec tmNow;
	clock_gettime(CLOCK_MONOTONIC, &tmNow);
	struct timespec* pTm = &pipes->tmStart->tm;
	return (tmNow.tv_sec - pTm->tv_sec) * 1000 + (tmNow.tv_nsec - pTm->tv_nsec) / 1000000;
#else
	using namespace std::chrono;
	time_point<steady_clock> tmNow = steady_clock::now();
	return duration_cast<milliseconds>(tmNow - pipes->tmStart->tm).count();
#endif
}
//
int timer_init_main_thread(struct pps_timer* tmr, struct pipes* pipes, uint32_t index);
int timer_deinit_main_thread(struct pps_timer* tmr);

#endif // !PPS_TIMER_H

