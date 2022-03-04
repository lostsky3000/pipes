
#include "pps_worker.h"
#include "pps_macro.h"
#include "pps_message.h"
#include "pps_service.h"
#include "pipes.h"
#include "pps_config.h"
#include "pool_linked.h"
#include "pps_timer.h"
#include <atomic>
#include <chrono>
#include <thread>

#include <cstdio>   // debug

static inline void proc_svc_msg_in(struct pps_message* m,
	struct pps_service* s, struct pps_service_ud* svcUd);
static inline void proc_timer_msg_in(struct timer_msg* m,
	struct pps_service* s,
	struct pps_service_ud* ud);
static inline void proc_net_msg_in(struct net_msg* m,
	struct pps_service* s,
	struct pps_service_ud* ud,
	struct pps_worker* wk);

static bool steal_task(struct pps_worker* workers,
	uint32_t workerNum, 
	uint32_t& lastStealWorker,
	uint32_t curWorkerIdx,
	int32_t* svcIdx);
static bool proc_svc(struct pps_service* svc, struct pps_worker* worker, 
	int32_t stepMsgMax, int32_t* procMsgNum=nullptr);

static void do_svc_free(struct pps_service* s, struct pipes* pipes)
{
	struct pps_service_ud* svcUd = &s->ud;
	struct pps_message msgIn;
	while (mpsc_pop(s->mqIn, &msgIn)) {
		proc_svc_msg_in(&msgIn, s, svcUd);
	}
	// clear all timer msg
	if (s->mqTimer) {
		struct timer_msg tmMsg;
		while (spsc_pop(s->mqTimer, &tmMsg)) {}
	}
	// clear all net msg
	if (s->mqNet) {
		struct net_msg netMsg;
		while (mpsc_pop(s->mqNet, &netMsg)) {}
	}
	if (s->exclusiveCtx) {
		delete s->exclusiveCtx;
		s->exclusiveCtx = nullptr;
	}
	// notify svc exit
	msg_gen_svc_exit(&msgIn);
	svcUd->cbOnMsg(&msgIn, svcUd->userData, nullptr);
	svcUd->cbOnMsg = nullptr;
	svcUd->userData = nullptr;
	svcUd->cbDeMsg = nullptr;
	svcUd->cbOnTimerMsg = nullptr;
	svcUd->cbOnNetMsg = nullptr;
	SVC_RT_END(s);
	svc_back(pipes, s->svcIdx);
}
void worker_thread(struct pps_worker* wkSelf)
{
	printf("worker(%d) start\n", wkSelf->index);
	//
	struct pipes* pipes = wkSelf->pipes;
	uint32_t workerNum = pipes->config->worker_num;
	uint32_t curWorkerIdx = wkSelf->index;
	uint32_t totalLoopNum = pipes->totalLoopThread.load();
	// incr worker started num
	pipes->loopStartedNum.fetch_add(1);
	// wait all workers started
	while(pipes->loopStartedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("worker(%d) will loop\n", wkSelf->index);
	//
	struct pps_worker* workers = pipes->workers;
	uint32_t lastStealWorker = 0;
	std::condition_variable& condNotifyWorker = pipes->condNotifyWorker;
	std::mutex& mtxNotifyWorker = pipes->mtxNotifyWorker;
	std::atomic<int32_t>& idleWorkerNum = pipes->idleWorkerNum;
	std::atomic<uint32_t>& totalWorkerTask = pipes->totalWorkerTask;
	struct mq_mpmc<int32_t>* queTask = wkSelf->queTask;
	struct pps_service_mgr* svcMgr = pipes->serviceMgr;
	struct pool_linked<struct pps_service*>* plkSvcWaitFree = wkSelf->plkSvcWaitFree;
	int32_t& svcWaitFreeNum = wkSelf->svcWaitFreeNum;
	struct pps_service* svc = nullptr;
	int32_t svcIdx = 0;
	bool svcNoBiz = true;
	if(curWorkerIdx == 0) { // 1st worker, proc boot
		pipes->ud.cb(pipes->ud.ud, pipes);
	}
	while(true) {
		if(svcWaitFreeNum > 0){   // has svc wait free
			const plk_iterator<struct pps_service*> it = plk_it_init(plkSvcWaitFree);
			struct pps_service* pSvc;
			for (; plk_it_loop(it);) {
				pSvc = **it;
				if(pSvc->calledCnt.load() == 0){
					plk_erase(it);
					--svcWaitFreeNum;
					do_svc_free(pSvc, pipes);
				}
			}	
		}
		//
		svc = nullptr;
		if (mpmc_pop(queTask, &svcIdx)) { //got task from curWorker
			if(svcIdx == -1){   // means shutdown
				break;
			}
			assert((svc=svcmgr_get_svc(svcMgr, svcIdx)) != nullptr);
		}
		else{   // // no task for curWorker, steal from others
			if(steal_task(workers, workerNum, lastStealWorker, curWorkerIdx, &svcIdx)) {  // steal succ
				assert((svc = svcmgr_get_svc(svcMgr, svcIdx)) != nullptr);
				svc->lastWorker.store(curWorkerIdx, std::memory_order_relaxed);      // update lastWorkerIdx
			}
		}
		//
		if(svc) {  //  found task, proc task msg
			svcNoBiz = proc_svc(svc, wkSelf, SVC_STEP_PROCMSG_MAX);
			//printf("proc task, idx=%d\n", svc->svcIdx); // debug
			if(svcNoBiz) { // change onTask to false
				assert(svc->onTask.exchange(false));
				if(!svc->exitCalled && 
						(mpsc_size(svc->mqIn) > 0 
						|| (svc->mqTimer && spsc_size(svc->mqTimer) > 0)
						|| (svc->mqNet && mpsc_size(svc->mqNet) > 0 ))) {
					 // still has biz to proc, re-add task
					bool preState = false;
					if (svc->onTask.compare_exchange_strong(preState, true)) {
						worker_add_task(pipes, svc, false, wkSelf);
					}
				}else { // not onTask, decr totalTask
					totalWorkerTask.fetch_sub(1, std::memory_order_relaxed);
					//uint32_t leftTask = totalWorkerTask.fetch_sub(1, std::memory_order_relaxed) - 1;
					//printf("task out, svcIdx=%d, leftTask=%d\n", svc->svcIdx, leftTask);
					//
					if(svc->exitCalled) { // svc exit, free slot
						if(svc->calledCnt.load() == 0){   // can free now
							do_svc_free(svc, pipes);
						}else{   // add to waitfree queue
							plk_push(plkSvcWaitFree, svc);
							++svcWaitFreeNum;
						}
					}
				}
			}
			else { // still has biz to proc, re-add task
				worker_add_task(pipes, svc, false, wkSelf);
			}
		}else if(svcWaitFreeNum < 1) { // no task found && no waitFreeSvc, check idle
			int32_t idleNum = idleWorkerNum.fetch_add(1) + 1;
			uint32_t totalTask = totalWorkerTask.load();
			if (totalTask <= workerNum - idleNum) {
				/*
				printf("worker(%d) will idle, totalTask=%d, idleWorker=%d\n", 
					curWorkerIdx,
					totalTask,
					idleWorkerNumNow);  // debug
					*/
				std::unique_lock<std::mutex> lock(mtxNotifyWorker);
				condNotifyWorker.wait(lock);
				//condNotifyWorker.wait_for(lock, std::chrono::milliseconds(waitTmout));
				//printf("worker(%d) idle finish\n", curWorkerIdx);  // debug
			}
			idleWorkerNum.fetch_sub(1);
			//printf("dddd thIdx=%d, totalTask=%d, idelNum=%d\n", curWorkerIdx, totalTask, idleWorkerNum);
		}
	}
	// wait all extthread done
	pps_wait_ext_thread_done(pipes);
	//
	exclusive_mgr_waitalldone(pipes->exclusiveMgr);
	//
	pipes->loopExitedNum.fetch_add(1);
	// wait all worker exit
	while(pipes->loopExitedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("worker(%d) end\n", wkSelf->index);
}

void worker_exclusive_thread(struct exclusive_thread_ctx* ctx)
{
	printf("exclusive worker start\n");   // debug
	struct pipes* pipes = ctx->pipes;
	struct pps_worker* worker = ctx->worker;
	struct pps_service* svc = ctx->svc;
	struct exclusive_svc_ctx* svcCtx = ctx->svcCtx;
	std::atomic<bool>& isIdle = svcCtx->isIdle;
	std::atomic<uint32_t>& msgNum = svcCtx->msgNum;
	std::mutex& mtx = svcCtx->mtx;
	std::condition_variable& cond = svcCtx->cond;
	std::atomic<bool>& loop = ctx->loop;
	worker_exclusive_init(worker, pipes);
	int32_t procMsgNum;
	bool svcNoBiz;
	while(loop.load(std::memory_order_relaxed)){
		svcNoBiz = proc_svc(svc, worker, -1, &procMsgNum);
		if(procMsgNum > 0){   // has proc some msg
			msgNum.fetch_sub(procMsgNum);
		}
		if(svcNoBiz){  // no biz, check exit or idle
			if (svc->exitCalled) {
				break;
			}
			isIdle.store(true);
			if(msgNum.load() < 1){   // really no msg, idle
				std::unique_lock<std::mutex> lock(mtx);
				cond.wait(lock);
			}
			isIdle.store(false);
		}
	}
	if(!svc->exitCalled){
		svc->exitCalled;
		svc->svcCnt.fetch_add(1);
	}
	while(svc->calledCnt.load() != 0){
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	do_svc_free(svc, pipes);
	exclusive_mgr_svcexit(ctx);
	worker_exclusive_deinit(worker);
	delete worker;
	delete ctx;
	printf("exclusive worker end\n");   // debug
}

static bool proc_svc(struct pps_service* svc, struct pps_worker* worker, int32_t stepMsgMax,
	int32_t* procMsgNumOut)
{
	if(procMsgNumOut){
		*procMsgNumOut = 0;
	}
	int32_t procMsgNum = 0;
	bool svcNoBiz = true;
	uint32_t inMsgLeft = 0;
	uint32_t tmMsgLeft = 0;
	uint32_t netMsgLeft = 0;
	SVC_RT_BEGIN(svc);
	struct pps_service_ud* svcUd = &svc->ud;
	svc->curWorker = worker;
	struct mq_mpsc<struct pps_message>* svcMqIn = svc->mqIn;
	bool& svcExitCalled = svc->exitCalled;
	//
	if (!svcExitCalled) { // not exit, can proc  msgIn, msgTimer, msgNet
		if (svc->mqTimer) {   // check timer msg
			struct timer_msg tmMsg;
			while (spsc_pop(svc->mqTimer, &tmMsg, &tmMsgLeft)) {
				proc_timer_msg_in(&tmMsg, svc, svcUd);
				++procMsgNum;
				if (svcExitCalled) { // exit or yield called
					break;
				}
			}
		}
		if (svc->mqNet && !svcExitCalled) {   // check net msg
			struct net_msg netMsg;
			if(stepMsgMax > 0){
				uint32_t tmpMsgNum = stepMsgMax;
				while (mpsc_pop_custom(svc->mqNet, &netMsg, net_msg_pop_fill, &netMsgLeft)) {
					proc_net_msg_in(&netMsg, svc, svcUd, worker);
					++procMsgNum;
					if (svcExitCalled) {
						break;
					}
					if (--tmpMsgNum < 1) {
						break;
					}
				}
			}else{
				while (mpsc_pop_custom(svc->mqNet, &netMsg, net_msg_pop_fill, &netMsgLeft)) {
					proc_net_msg_in(&netMsg, svc, svcUd, worker);
					++procMsgNum;
					if (svcExitCalled) {
						break;
					}
				}
			}
		}
		if (!svcExitCalled) {   // proc msgIn
			struct pps_message msgIn;
			if (mpsc_pop(svcMqIn, &msgIn, &inMsgLeft)) {
				proc_svc_msg_in(&msgIn, svc, svcUd);
				++procMsgNum;
				if (inMsgLeft > 0 && !svcExitCalled) {  // has left msg & not exit
					if(stepMsgMax > 0){  // stepMsgMax valid
						int32_t tmpMsgNum = stepMsgMax - 1;
						while (mpsc_pop(svcMqIn, &msgIn, &inMsgLeft)) {   // proc msg
							proc_svc_msg_in(&msgIn, svc, svcUd);
							++procMsgNum;
							if (svcExitCalled) { // exit or yield called
								break;
							}
							if (--tmpMsgNum < 1) {
								break;
							}
						}
					}else{   // consume all
						while (mpsc_pop(svcMqIn, &msgIn, &inMsgLeft)) {   // proc msg
							proc_svc_msg_in(&msgIn, svc, svcUd);
							++procMsgNum;
							if (svcExitCalled) { // exit or yield called
								break;
							}
						}
					}
				}
			}
		}
	}
	// proc waitSend msg
	if (svc->waitSendMsgNum > 0) { // has waitSend msg
		const plk_iterator<struct pps_message> it = plk_it_init(svc->mqWaitSend);
		struct pps_message* pMsg;
		if(stepMsgMax > 0){
			int32_t tmpMsgNum = stepMsgMax;
			for (; plk_it_loop(it);) {
				pMsg = *it;
				if (svcExitCalled && pMsg->session > 0) { // req of call & svcExitCalled, can destroy
					svcUd->cbDeMsg(pMsg, svcUd->userData);
					plk_erase(it);
					--svc->waitSendMsgNum;
					--tmpMsgNum;
				} else {
					if (svc_sendmsg(idxpair_decode(pMsg->idx_pair), pMsg, worker->pipes, nullptr) > 0) {
						plk_erase(it);
						--svc->waitSendMsgNum;
						--tmpMsgNum;
					}
				}
				if (tmpMsgNum < 1) {
					break;
				}
			}
		}else{  // consume all
			for (; plk_it_loop(it);) {
				pMsg = *it;
				if (svcExitCalled && pMsg->session > 0) { // req of call & svcExitCalled, can destroy
					svcUd->cbDeMsg(pMsg, svcUd->userData);
					plk_erase(it);
					--svc->waitSendMsgNum;
				} else {
					if (svc_sendmsg(idxpair_decode(pMsg->idx_pair), pMsg, worker->pipes, nullptr) > 0) {
						plk_erase(it);
						--svc->waitSendMsgNum;
					}
				}
			}
		}
	}
	if ((!svcExitCalled && (inMsgLeft > 0 || tmMsgLeft > 0 || netMsgLeft > 0))
		|| svc->waitSendMsgNum > 0) {
		svcNoBiz = false;
	}
	SVC_RT_END(svc);
	if (procMsgNumOut) {
		*procMsgNumOut = procMsgNum;
	}
	return svcNoBiz;
}

int worker_add_task(struct pipes* pipes, struct pps_service*s, bool isNew, struct pps_worker* wkSelf)
{
	assert(isNew || wkSelf);
	if (isNew) {
		int32_t idxWorker = s->lastWorker.load(std::memory_order_relaxed);
		if (idxWorker < 0) { // no worker before, alloc one
		    idxWorker = pipes->svcWorkerCnt.fetch_add(1, std::memory_order_relaxed)
		   		% pipes->config->worker_num;
			s->lastWorker.store(idxWorker, std::memory_order_relaxed);
		}
		//printf("worker add new task, worker=%d\n", idxWorker);    // debug
		// add to dst worker
		assert(mpmc_push(pipes->workers[idxWorker].queTask, s->svcIdx));
		// incr totalTask
		int32_t totalTask = pipes->totalWorkerTask.fetch_add(1) + 1;
		// check notify worker thread
		int32_t idleWorkerNum = pipes->idleWorkerNum.load();
		if (idleWorkerNum > 0 && totalTask > pipes->config->worker_num - idleWorkerNum) {
			//printf("addTask, will notify. totalTask=%d, idleWorker=%d\n", totalTask, idleWorkerNum);
			// has idleWorkerThread, notify
			std::unique_lock<std::mutex> lock(pipes->mtxNotifyWorker);
			pipes->condNotifyWorker.notify_one();
		}
		else {
			//printf("addTask, no notify. totalTask=%d, idleWorker=%d\n", totalTask, idleWorkerNum);
		}
	}
	else {
		//printf("worker add exist task\n");   // debug
		assert(mpmc_push(wkSelf->queTask, s->svcIdx));
	}
	return 1;
}

int worker_shutdown(struct pipes* pipes)
{
	for(int i=0; i<pipes->config->worker_num; ++i) {
		struct pps_worker* wk = &pipes->workers[i];
		assert(mpmc_push(wk->queTask, -1));
	}
	{
		std::unique_lock<std::mutex> lock(pipes->mtxNotifyWorker);
		pipes->condNotifyWorker.notify_all();
	}
	return 1;
}

static inline void proc_timer_msg_in(struct timer_msg* m,
	struct pps_service* s,
	struct pps_service_ud* ud)
{
	if (m->svcCnt != s->svcCntLocal) {   //  svc has gone
		return ;
	}
	ud->cbOnTimerMsg(m, ud->userData, s);
}

static inline void proc_net_msg_in(struct net_msg* m,
	struct pps_service* s,
	struct pps_service_ud* ud,
	struct pps_worker* wk)
{
	if (m->svcCnt != s->svcCntLocal) { //  svc has gone
		return;
	}
	ud->cbOnNetMsg(m, ud->userData, s, wk);
}

static inline void proc_svc_msg_in(struct pps_message* m,
	struct pps_service* s,
	struct pps_service_ud* ud)
{
	if (m->to_cnt != s->svcCntLocal) { // svcCnt not match, cant consume
		if(m->session > 0) {   // is call-req, need response
			uint32_t idxEcho;
			idxpair_decode(m->idx_pair, &idxEcho);
			struct pps_message mEcho;
			mEcho.from_cnt = 0;
			mEcho.to_cnt = m->from_cnt;
			mEcho.idx_pair = idxpair_encode(0, idxEcho);
			mEcho.session = -m->session;
			mEcho.data = nullptr;
			mEcho.size = msg_size_combine(MTYPE_SVC_GONE, 0);
			if (svc_sendmsg(idxEcho, &mEcho, s->pipes, s) > 0) {
			
			}
		}
		// destroy msg
		ud->cbDeMsg(m, ud->userData);
		return;
	}
	ud->cbOnMsg(m, ud->userData, s);
}
static bool steal_task(struct pps_worker* workers,
	uint32_t workerNum, 
	uint32_t& lastStealWorker,
	uint32_t curWorkerIdx,
	int32_t* svcIdx)
{
	for (uint32_t i = 0; i < workerNum; ++i) {
		if (lastStealWorker != curWorkerIdx) {
			if (mpmc_pop(workers[lastStealWorker].queTask, svcIdx)) {
				 // steal succ
				if(++lastStealWorker >= workerNum) {
					 // move steal idx
					lastStealWorker = 0;
				}
				return true;
			}
		}
		if (++lastStealWorker >= workerNum) {
			 // move steal idx
			lastStealWorker = 0;
		}
	}	
	return false;
}


//
int worker_init_main_thread(struct pps_worker* wk, struct pipes* pipes, uint32_t index)
{
	wk->pipes = pipes;
	wk->index = index;
	wk->queTask = const_cast<struct mq_mpmc<int32_t>*>(mpmc_create<int32_t>(SVC_SLOT_NUM));
	wk->tmpBuf.cap = WORKER_TMP_BUF_SIZE;
	wk->tmpBuf.buf = (char*)pps_malloc(wk->tmpBuf.cap);
	wk->arrPtrChar = nullptr;
	wk->arrPtrCharSize = 0;
	wk->arrInt = nullptr;
	wk->arrIntSize = 0;
	wk->plkSvcWaitFree = const_cast<struct pool_linked<struct pps_service*>*>(plk_create<struct pps_service*>());
	wk->svcWaitFreeNum = 0;
	return 1;
}
int worker_deinit_main_thread(struct pps_worker* wk)
{
	mpmc_destroy(wk->queTask);
	pps_free(wk->tmpBuf.buf);
	plk_destroy(wk->plkSvcWaitFree);
	return 1;	
}

int worker_exclusive_init(struct pps_worker* wk, struct pipes* pipes)
{
	wk->pipes = pipes;
	wk->index = 0;
	wk->queTask = nullptr;
	wk->tmpBuf.cap = WORKER_TMP_BUF_SIZE;
	wk->tmpBuf.buf = (char*)pps_malloc(wk->tmpBuf.cap);
	wk->arrPtrChar = nullptr;
	wk->arrPtrCharSize = 0;
	wk->arrInt = nullptr;
	wk->arrIntSize = 0;
	return 1;
}
int worker_exclusive_deinit(struct pps_worker* wk)
{
	pps_free(wk->tmpBuf.buf);
	return 1;
}