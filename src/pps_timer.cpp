#include "pps_timer.h"
#include "pipes.h"
#include "pps_sysapi.h"
#include "pps_message.h"
#include "pps_service.h"
#include <thread>
#include <chrono>

#define TIMER_QUEUE_LEN 4096
#define TIMER_TASK_POOL_CAP 4096

#define TIMER_TASK_SLOT_NUM 65536
#define TIMER_MAX_TASK TIMER_TASK_SLOT_NUM-1000

struct timer_task
{
	int32_t poolIdx;
	struct pps_timer* timer;
	struct timer_msg msg;
};

static inline void cond_init(struct timer_cond& cond);
static inline void cond_notify(struct timer_cond& cond);
static inline void cond_waitfor(struct timer_cond& cond,
	int64_t delay, int64_t tmNow,
	struct pipes* pipes, int64_t& dbgCost);
static inline void cond_deinit(struct timer_cond& cond);

static void thread_work(struct pps_timer* tmr)
{
	printf("timer(%d) start\n", tmr->index);
	struct pipes* pipes = tmr->pipes;
	uint32_t totalLoopNum = pipes->totalLoopThread.load();
	std::atomic<bool>& idle = tmr->idle;
	idle.store(false);
	//
	cond_init(tmr->idleCond);
	// incr loop started num
	pipes->loopStartedNum.fetch_add(1);
	// wait all loop started
	while(pipes->loopStartedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("timer(%d) will loop\n", tmr->index);
	//
	struct tw_timer<struct timer_task*>* twTimer = tmr->twTimer;
	struct mq_mpsc<int32_t>* queTask = tmr->queTask;
	struct pool_linked<struct timer_task*>* plkReAddTask = tmr->plkReAddTask;
	struct pool_linked<struct timer_msg>* plkUnRspTask = tmr->plkUnRspTask;
	uint32_t& reAddTaskNum = tmr->reAddTask;
	uint32_t& unRspTaskNum = tmr->unRspTask;
	std::atomic<uint32_t>& taskBakNum = tmr->taskBakNum;
	struct timer_msg* pMsg;
	struct timer_task* pTask;
	struct timer_task* pTaskSlot = tmr->taskSlots;
	std::atomic<uint32_t>& unAddTask = tmr->unAddTask;
	struct timer_cond& idleCond = tmr->idleCond;
	int64_t tmNow, delay;
	int32_t addCnt, reAddCnt, reRspCnt;
	int32_t idxTask;
	const int32_t stepMsgMax = 2000;
	bool shutdown = false;
	// debug
	int64_t tmDbg1, tmDbg2, tmDbg3, tmDbg4, 
		dbgLastDelay, dbgLastDelayReal, dbgTmLastAdvance, 
		dbgDelayCost, dbgDelayCost2;
	// start loop
	while(true) {
		reAddCnt = 0;
		tmNow = timer_clock_now_ms(pipes);
		// tm debug begin
		tmDbg1 = tmNow;
		tmDbg2 = tmDbg3 = tmDbg4 = 0;
		// tm debug end
		// check reAdd task
		if(reAddTaskNum > 0) {
			while (plk_pop(plkReAddTask, &pTask)) {
				--reAddTaskNum;
				++reAddCnt;
				//tmNow = sysapi_clock_now_ms(); // debug
				tmwheel_add(pTask->msg.expiration,
					tmNow,
					pTask,
					twTimer);
			}
			tmNow = timer_clock_now_ms(pipes);
			tmDbg2 = tmNow;   // debug
		}
		// check unRsp task
		reRspCnt = 0;
		if (unRspTaskNum > 0) {
			const plk_iterator<struct timer_msg> it = plk_it_init(plkUnRspTask);
			for (; plk_it_loop(it);) {
				pMsg = *it;
				int32_t ret = svc_send_timermsg(pMsg->svcIdx, pMsg, tmr);
				if (ret != 0) {
					   // send succ || dst has gone
					plk_erase(it);
					--unRspTaskNum;
				}
				++reRspCnt;
			}
			tmNow = timer_clock_now_ms(pipes);
			tmDbg3 = tmNow;   // debug
		}
		// check task queue
		addCnt = 0;
		while (mpsc_pop(queTask, &idxTask)) {
			if(idxTask == -1){
				shutdown = true;
				break;
			}
			pTask = &pTaskSlot[idxTask];
			//tmNow = sysapi_clock_now_ms();  // debug
			tmwheel_add(pTask->msg.expiration, tmNow, pTask, twTimer);
			if (++addCnt >= stepMsgMax) {
				break;
			}
		}
		if(shutdown){
			break;
		}
		if (taskBakNum.load(std::memory_order_relaxed) > 0) { // has bak task
			uint32_t tNum = 0;
			while (true) {
				pTask = nullptr;
				{
					std::unique_lock<std::mutex> lock(tmr->mtxTaskBak);
					plk_pop(tmr->plkTaskBak, &pTask);
				}
				if (pTask) {
					++tNum;
					//tmNow = sysapi_clock_now_ms();  // debug
					tmwheel_add(pTask->msg.expiration, tmNow, pTask, twTimer);
					if (++addCnt >= stepMsgMax + stepMsgMax) {
						break;
					}
				}
				else {
					break;
				}
			}
			taskBakNum.fetch_sub(tNum, std::memory_order_relaxed);
		}
		//
		if(addCnt > 0 || reAddCnt > 0 || reRspCnt > 0) {
			unAddTask.fetch_sub(addCnt);
			tmNow = timer_clock_now_ms(pipes);    // has act, update tmNow for accurate
			tmDbg4 = tmNow;  // debug
		}
		//tmNow = sysapi_clock_now_ms();  // debug
		// advance clock
		delay = tmwheel_advance_clock(tmNow, twTimer);
		dbgLastDelay = delay;     // debug
		dbgTmLastAdvance = tmNow;   // debug
		//
		if(addCnt < 1 && reAddTaskNum < 1 && unRspTaskNum < 1) {
			// no act this loop, check idle
			idle.store(true);
			if (unAddTask.load() < 1) {
				if (delay <= 0 || delay >= 10000) {
					delay = 10000;
				}
				int64_t tm1 = timer_clock_now_ms(pipes);      // debug
				cond_waitfor(idleCond, delay, tmNow, pipes, dbgDelayCost2);
				dbgDelayCost = timer_clock_now_ms(pipes) - tm1;    // debug
			}
			idle.store(false);
		}
	}
	// wait all extthread done
	pps_wait_ext_thread_done(pipes);
	// decr loopExited num
	pipes->loopExitedNum.fetch_add(1);
	// wait all loop exit
	while(pipes->loopExitedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	//
	cond_deinit(tmr->idleCond);
}

#if defined(USE_LINUX_TIMER)
void* timer_thread(void* arg)
{
	struct pps_timer* tmr = (struct pps_timer*)arg;
	thread_work(tmr);
	return NULL;
}
#else
void timer_thread(struct pps_timer* tmr)
{
	thread_work(tmr);
}
#endif

static void on_timeout(struct timer_task* task, int64_t tmNow)	
{
	//printf("timer exec on_timeout\n");  // debug
	struct pps_timer* tmr = task->timer;
	struct timer_msg* m = &task->msg;
	m->session = -m->session;
	
	// debug begin
	int64_t realNow = timer_clock_now_ms(tmr->pipes);    
	int64_t off = realNow - m->expiration;  
	if(off > 4) {
		printf("timeout delayed, off=%ld\n", off);
	}
	m->expiration = tmNow;  
	// debug end
	
	int32_t ret = svc_send_timermsg(m->svcIdx, m, tmr);
	if (ret == 0) {  // dst mq is full, add to waitsend
		plk_push_ptr(tmr->plkUnRspTask, m);
		++tmr->unRspTask;
	}
	if (--m->repeat >= 0 && ret >= 0) {  //  schedule not complete, readd		
		m->session = -m->session;
		m->expiration = tmNow + m->delay;
		++tmr->reAddTask;
		plk_push(tmr->plkReAddTask, task);
	}
	else {  //back task
		if(task->poolIdx >= 0) {  // from taskPool
			mpmc_push(tmr->freeTaskPool, task->poolIdx);
		}else {   // from taskBak
			delete task;
		} 
	}
}
int timer_add_task(struct timer_msg*m, struct pps_timer* timer)
{
	int32_t idxTask;
	if (mpmc_pop(timer->freeTaskPool, &idxTask)) {
		struct timer_task* t = &timer->taskSlots[idxTask];
		t->msg = *m;
		mpsc_push(timer->queTask, idxTask);
	}
	else {   // no free slot
		struct timer_task* t = new struct timer_task;
		t->poolIdx = -1;
		t->timer = timer;
		t->msg = *m;
		{
			std::unique_lock<std::mutex> lock(timer->mtxTaskBak);
			plk_push(timer->plkTaskBak, t);
		}
		timer->taskBakNum.fetch_add(1);
	}
	//
	timer->unAddTask.fetch_add(1);
	if (timer->idle.load()) {// notify
		 cond_notify(timer->idleCond);
	}
	return 1;	
}

void timer_shutdown(struct pipes* pipes)
{
	for(int i=0; i<pipes->config->timer_num; ++i) {
		struct pps_timer* timer = &pipes->timers[i];
		while(!mpsc_push(timer->queTask, -1)){
		
		}
		cond_notify(timer->idleCond);
	}
}

//
static inline void cond_init(struct timer_cond& cond)
{
#ifdef USE_LINUX_TIMER
	pthread_mutex_init(&cond.mtx, NULL);
	assert(pthread_condattr_init(&cond.condAttr) == 0);
	assert(pthread_condattr_setclock(&cond.condAttr, CLOCK_MONOTONIC) == 0);
	assert(pthread_cond_init(&cond.cond, &cond.condAttr) == 0);
#endif
}
static inline void cond_deinit(struct timer_cond& cond)
{
#ifdef USE_LINUX_TIMER
	pthread_mutex_destroy(&cond.mtx);
	pthread_condattr_destroy(&cond.condAttr);
	pthread_cond_destroy(&cond.cond);
#endif
}
static inline void cond_notify(struct timer_cond& cond)
{
#ifdef USE_LINUX_TIMER
	pthread_mutex_lock(&cond.mtx);
	pthread_cond_signal(&cond.cond);
	pthread_mutex_unlock(&cond.mtx);
#else
	std::unique_lock<std::mutex> lock(cond.mtx);
	cond.cv.notify_one();
#endif
}

static inline void cond_waitfor(struct timer_cond& cond, int64_t delay, 
	int64_t tmNow, struct pipes* pipes, int64_t& dbgCost)
{
#ifdef USE_LINUX_TIMER
	delay -= 1;
	if (delay <= 0) {
		dbgCost = 0;
		return;
	}
	struct timespec ts;
	//struct timespec ts2, ts3;   // debug
	//
	struct timespec* pTmStart = &pipes->tmStart->tm;
	ts.tv_sec = pTmStart->tv_sec;
	ts.tv_nsec = pTmStart->tv_nsec;
	int64_t ns = ts.tv_nsec + (tmNow + delay) * 1000000;
	ts.tv_nsec = ns % 1000000000;
	ts.tv_sec += ns / 1000000000;
	pthread_mutex_t* pMtx = &cond.mtx;
	pthread_mutex_lock(pMtx);
	
	//clock_gettime(CLOCK_MONOTONIC, &ts2);   // debug
	pthread_cond_timedwait(&cond.cond, pMtx, &ts);
	//clock_gettime(CLOCK_MONOTONIC, &ts3);   //   debug
	//dbgCost = (ts3.tv_sec - ts2.tv_sec) * 1000 + (ts3.tv_nsec - ts2.tv_nsec) / 1000000;  //  debug
	
	pthread_mutex_unlock(pMtx);
#else
	std::unique_lock<std::mutex> lock(cond.mtx);
	cond.cv.wait_for(lock, std::chrono::milliseconds(delay));
#endif
}

//
int timer_init_main_thread(struct pps_timer* tmr, struct pipes* pipes, uint32_t index)
{
	tmr->pipes = pipes;
	tmr->index = index;
	tmr->unAddTask.store(0);
	//
	tmr->reAddTask = 0;
	tmr->plkReAddTask = const_cast<struct pool_linked<struct timer_task*>*>(plk_create<struct timer_task*>(TIMER_MAX_TASK));
	//
	tmr->unRspTask = 0;
	tmr->plkUnRspTask = const_cast<struct pool_linked<struct timer_msg>*>(plk_create<struct timer_msg>(4096));
	//
	tmr->taskBakNum.store(0);
	tmr->plkTaskBak = const_cast<struct pool_linked<struct timer_task*>*>(plk_create<struct timer_task*>(4096));
	//
	tmr->twTimer = tmwheel_create<struct timer_task*>(
		2,
		500,
		0,
		on_timeout,
		10000,
		10000);
	//
	tmr->taskSlots = new struct timer_task[TIMER_TASK_SLOT_NUM];
	tmr->freeTaskPool = const_cast<struct mq_mpmc<int32_t>*>(mpmc_create<int32_t>(TIMER_TASK_SLOT_NUM));
	tmr->queTask = const_cast<struct mq_mpsc<int32_t>*>(mpsc_create<int32_t>(TIMER_TASK_SLOT_NUM));
	for (int32_t i = 0; i < TIMER_TASK_SLOT_NUM; ++i) {
		struct timer_task* t = &tmr->taskSlots[i];
		t->poolIdx = i;
		t->timer = tmr;
		if (i < TIMER_MAX_TASK) {
			mpmc_push(tmr->freeTaskPool, i);
		}
	}
	return 1;	
}
int timer_deinit_main_thread(struct pps_timer* tmr)
{
	delete[] tmr->taskSlots;
	plk_destroy(tmr->plkReAddTask);
	plk_destroy(tmr->plkUnRspTask);
	plk_destroy(tmr->plkTaskBak);
	mpmc_destroy(tmr->freeTaskPool);
	mpsc_destroy(tmr->queTask);
	//
	tmwheel_destroy(tmr->twTimer);
	return 1;	
}

