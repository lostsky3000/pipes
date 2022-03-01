
#include "pps_logger.h"
#include "pipes.h"
#include <thread>
#include <chrono>
#include <cstring>
#include "pps_sysapi.h"
#include "pps_malloc.h"

#define LOGGER_POOL_CAP 4096

static inline int try_log(struct logger_msg* m, uint32_t& idCnt)	
{
	const char chLevel[4] = { 'D','I','W','E' };
	if (idCnt == m->id) { // id match, do log
		++idCnt;
		int32_t tmSec = m->when / 1000;
		printf("%d [LOGGER][%c] %s\n", tmSec, chLevel[m->level], m->msg);
		return 1;
	}
	return 0;
}

void logger_thread(struct pps_logger* log)
{
	struct pipes* pipes = log->pipes;
	uint32_t totalLoopNum = pipes->totalLoopThread.load();
	std::atomic<bool>& idle = log->idle;
	idle.store(false);
	// incr loop started num
	pipes->loopStartedNum.fetch_add(1);
	// wait all loop started
	while(pipes->loopStartedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("logger will loop\n");
	//
	std::atomic<uint32_t>& unAddMsgNum = log->unAddMsgNum;
	mq_mpsc<struct logger_msg*>* mqMsg = log->mqMsg;
	mq_mpmc<struct logger_msg*>* mqMsgPool = log->mqMsgPool;
	std::atomic<bool>& loop = log->loop;
	loop.store(true);
	struct logger_msg* pMsg;
	uint32_t idCnt = 0;
	uint32_t addCnt;
	pool_linked<struct logger_msg*>* plkWaitLog = 
		const_cast<struct pool_linked<struct logger_msg*>*>(plk_create<struct logger_msg*>(4096));
	//
	while(loop.load(std::memory_order_relaxed)) {
		addCnt = 0;
		// check mqMsg
		while (mpsc_pop(mqMsg, &pMsg)) {
			++addCnt;
			if (try_log(pMsg, idCnt)) { // log succ, free to pool
				if(!mpmc_push(mqMsgPool, pMsg)) {  // pool is full, release
					pps_free(pMsg->msg);
					pps_free(pMsg);
				}
			}
			else {   // log failed, add to wait log
				plk_push(plkWaitLog, pMsg);
			}
		}
		// check mqMsgBak
		{
			std::unique_lock<std::mutex> lock(log->mtxMsgBak);
			while (plk_pop(log->plkMsgBak, &pMsg)) {
				++addCnt;
				plk_push(plkWaitLog, pMsg);
			}
		}
		// check waitLog
		const plk_iterator<struct logger_msg*> it = plk_it_init(plkWaitLog);
		for (; plk_it_loop(it);) {
			pMsg = *(*it);
			if (try_log(pMsg, idCnt)) { // log succ, free to pool
				plk_erase(it);
				if (!mpmc_push(mqMsgPool, pMsg)) {  // pool is full, release
					pps_free(pMsg->msg);
					pps_free(pMsg);
				}
			}
		}
		//
		if(addCnt > 0) {   // has add op
			unAddMsgNum.fetch_sub(addCnt);
		}
		//check idle
		if(plk_size(plkWaitLog) < 1) {
			idle.store(true);
			if (unAddMsgNum.load() < 1) {
				std::unique_lock<std::mutex> lock(log->mtxIdle);
				if (loop.load(std::memory_order_relaxed)) {
					log->condIdle.wait_for(lock, std::chrono::milliseconds(20000));
				}
			}
			idle.store(false);
		}
	}
	// decr loopExited num
	pipes->loopExitedNum.fetch_add(1);
	// wait all loop exit
	while(pipes->loopExitedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	const plk_iterator<struct logger_msg*> it = plk_it_init(plkWaitLog);
	for (; plk_it_loop(it);) {
		pMsg = *(*it);
		pps_free(pMsg->msg);
	}
	plk_destroy(plkWaitLog);
}

struct logger_msg* logger_send_begin(uint32_t size, int64_t who, int level, struct pps_logger*logger)
{
	uint32_t id = logger->msgIdCnt.fetch_add(1, std::memory_order_relaxed);
	int64_t tmNow = sysapi_system_now();
	// alloc msg
	struct logger_msg* pMsg;
	if (mpmc_pop(logger->mqMsgPool, &pMsg)) {
		if (size + 1 > pMsg->cap) { // realloc
			pps_free(pMsg->msg);
			pMsg->cap = size + 1;
			pMsg->msg = (char*)pps_malloc(pMsg->cap);
		}
		pMsg->msgLen = size;
	}
	else {
		pMsg = new struct logger_msg;
		pMsg->msgLen = size;
		pMsg->cap = size + 1;
		pMsg->msg = (char*)pps_malloc(pMsg->cap);
	}
	// fill msg
	pMsg->id = id;
	pMsg->who = who;
	pMsg->level = level;
	pMsg->when = tmNow;
	pMsg->msg[size] = '\0';
	return pMsg;
}
int logger_send_end(const char* str, struct logger_msg* pMsg, struct pps_logger*logger)
{
	memcpy(pMsg->msg, str, pMsg->msgLen);
	// enqueue
	if(!mpsc_push(logger->mqMsg, pMsg)) {
		std::unique_lock<std::mutex> lock(logger->mtxMsgBak);
		plk_push(logger->plkMsgBak, pMsg);
	}
	// check notify
	logger->unAddMsgNum.fetch_add(1);
	if (logger->idle.load()) {
		std::unique_lock<std::mutex> lock(logger->mtxIdle);
		logger->condIdle.notify_one();
	}
	return 1;	
}

void logger_shutdown(struct pipes* pipes)
{
	pipes->logger->loop.store(false);
	{
		std::unique_lock<std::mutex> lock(pipes->logger->mtxIdle);
		pipes->logger->condIdle.notify_all();
	}
}

//
void logger_init_main_thread(struct pps_logger* log, struct pipes* pipes)
{
	log->pipes = pipes;
	log->mqMsgPool = 
		const_cast<struct mq_mpmc<struct logger_msg*>*>(mpmc_create<struct logger_msg*>(LOGGER_POOL_CAP));
	for (int i=0; i<LOGGER_POOL_CAP; ++i) {
		struct logger_msg* m = new struct logger_msg;
		m->msgLen = 0;
		m->cap = 256;
		m->msg = (char*)pps_malloc(m->cap);
		assert(mpmc_push(log->mqMsgPool, m));
	}
	log->mqMsg = 
		const_cast<struct mq_mpsc<struct logger_msg*>*>(mpsc_create<struct logger_msg*>(4096));
	log->unAddMsgNum.store(0);
	log->msgIdCnt.store(0);
	{
		std::unique_lock<std::mutex> lock(log->mtxMsgBak);
		log->plkMsgBak = 
			const_cast<struct pool_linked<struct logger_msg*>*>(plk_create<struct logger_msg*>(4096));
	}
}

void logger_deinit_main_thread(struct pps_logger* log)
{
	mpmc_destroy(log->mqMsgPool);
	mpsc_destroy(log->mqMsg);
	{
		std::unique_lock<std::mutex> lock(log->mtxMsgBak);
		plk_destroy(log->plkMsgBak);
	}
}

/*
int logger_send(const char* str, uint32_t strLen, int64_t who, struct pps_logger* logger)
{
	uint32_t id = logger->msgIdCnt.fetch_add(1, std::memory_order_relaxed);
	// alloc msg
	struct logger_msg* pMsg;
	if (mpmc_pop(logger->mqMsgPool, &pMsg)) {
		if (strLen + 1 > pMsg->cap) {    // realloc
			pps_free(pMsg->msg);
			pMsg->cap = strLen + 1;
			pMsg->msg = (char*)pps_malloc(pMsg->cap);
		}
		pMsg->msgLen = strLen;
	}
	else {
		pMsg = new struct logger_msg;
		pMsg->msgLen = strLen;
		pMsg->cap = strLen + 1;
		pMsg->msg = (char*)pps_malloc(pMsg->cap);
	}
	// fill msg
	pMsg->id = id;
	pMsg->who = who;
	pMsg->when = sysapi_system_now();
	memcpy(pMsg->msg, str, strLen);
	pMsg->msg[strLen] = '\0';
	// enqueue
	if(!mpsc_push(logger->mqMsg, pMsg)) {
		std::unique_lock<std::mutex> lock(logger->mtxMsgBak);
		plk_push(logger->plkMsgBak, pMsg);
	}
	// check notify
	logger->unAddMsgNum.fetch_add(1);
	if (logger->idle.load()) {
		std::unique_lock<std::mutex> lock(logger->mtxIdle);
		logger->condIdle.notify_one();
	}
	return 1;
}
*/

