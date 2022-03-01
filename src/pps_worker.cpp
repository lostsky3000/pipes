
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

static int64_t calc_idlewait_tmout(uint32_t wkIdx, uint32_t workerNum);

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
	uint32_t curWorkerIdxPlus = curWorkerIdx + 1;
	uint32_t lastStealWorker = 0;
	std::atomic<uint32_t>& atomTotalTask = pipes->totalTask;
	struct mq_mpmc<int32_t>* queTask = wkSelf->queTask;
	struct pps_service_mgr* svcMgr = pipes->serviceMgr;
	struct pps_service* svc = nullptr;
	struct pps_message msg;
	struct pps_message* pMsg = &msg;
	struct timer_msg tmMsg;
	struct net_msg netMsg;
	struct mq_mpsc<struct pps_message>* svcMqIn = nullptr;
	struct pps_service_ud* svcUd;
	int32_t svcIdx = 0;
	bool svcNoBiz = true;
	uint32_t tmpU, inMsgLeft, tmMsgLeft, netMsgLeft;
	int32_t tmpMsgNum;
	//
	int64_t waitTmout = 360000; //calc_idlewait_tmout(curWorkerIdx, workerNum);
	// 
	if(curWorkerIdx == 0) { // 1st worker, proc boot
		pipes->ud.cb(pipes->ud.ud, pipes);
	}
	//
	while(true) {
		svc = nullptr;
		if (mpmc_pop(queTask, &svcIdx)) { //got task from curWorker
			if(svcIdx == -1){   // means shutdown
				break;
			}
			assert(svc = svcmgr_get_svc(svcMgr, svcIdx));
		}
		else{   // // no task for curWorker, steal from others
			if(steal_task(workers, workerNum, lastStealWorker, curWorkerIdx, &svcIdx)) {  // steal succ
				assert(svc = svcmgr_get_svc(svcMgr, svcIdx));
				svc->lastWorker.store(curWorkerIdx, std::memory_order_relaxed);      // update lastWorkerIdx
			}
		}
		// 
		if(svc) {  //  found task, proc task msg
			//printf("proc task, idx=%d\n", svc->svcIdx); // debug
			svcNoBiz = true;
			inMsgLeft = 0;
			tmMsgLeft = 0;
			netMsgLeft = 0;
			SVC_RT_BEGIN(svc);
			svcUd = &svc->ud;
			svc->curWorker = wkSelf;
			svcMqIn = svc->mqIn;
			bool& svcExitCalled = svc->exitCalled;
			//
			if(!svcExitCalled) { // not exit, can proc  msgIn, msgTimer, msgNet
				if(svc->mqTimer) {   // check timer msg
					while(spsc_pop(svc->mqTimer, &tmMsg, &tmMsgLeft)) {
						proc_timer_msg_in(&tmMsg, svc, svcUd);
						if (svcExitCalled) { // exit or yield called
						   break;
						}
					}
				}
				if (svc->mqNet && !svcExitCalled) {   // check net msg
					tmpMsgNum = 128;
					while (mpsc_pop_custom(svc->mqNet, &netMsg, net_msg_pop_fill, &netMsgLeft)) {
						proc_net_msg_in(&netMsg, svc, svcUd, wkSelf);
						if (svcExitCalled) {
							break;
						}
						if(--tmpMsgNum < 1){
							break;
						}
					}
				}
				if(!svcExitCalled){   // proc msgIn
					if(mpsc_pop(svcMqIn, pMsg, &inMsgLeft)){
						proc_svc_msg_in(pMsg, svc, svcUd);
						if(inMsgLeft > 0 && !svcExitCalled){  // has left msg & not exit
							// calc step consume msg num
							tmpMsgNum = inMsgLeft * curWorkerIdxPlus / workerNum;
							if (tmpMsgNum < 1) {
								tmpMsgNum = 1;
							}
							else if (tmpMsgNum > SVC_STEP_PROCMSG_MAX - 1) {
								tmpMsgNum = SVC_STEP_PROCMSG_MAX - 1;
							}
							// proc msgIn
							while(mpsc_pop(svcMqIn, pMsg, &inMsgLeft)) {   // proc msg
								proc_svc_msg_in(pMsg, svc, svcUd);
								if (svcExitCalled) { // exit or yield called
									break;
								}
								if (--tmpMsgNum < 1) {
									break;
								}
							}
						}
					}
				}
			}
			// proc waitSend msg
			if(svc->waitSendMsgNum > 0) { // has waitSend msg
				tmpMsgNum = svc->waitSendMsgNum * curWorkerIdxPlus / workerNum;
				if (tmpMsgNum < 1) {
					tmpMsgNum = 1;
				}
				else if (tmpMsgNum > SVC_STEP_PROCMSG_MAX) {
					tmpMsgNum = SVC_STEP_PROCMSG_MAX;
				}
				const plk_iterator<struct pps_message> it = plk_it_init(svc->mqWaitSend);
				for (; plk_it_loop(it);) {
					pMsg = *it;
					if (svcExitCalled && pMsg->session > 0) { // req of call & svcExitCalled, can destroy
						svcUd->cbDeMsg(pMsg, svcUd->userData);
						plk_erase(it);
						--svc->waitSendMsgNum;
						--tmpMsgNum;
					}
					else {  
						if (svc_sendmsg(idxpair_decode(pMsg->idx_pair), pMsg, pipes, nullptr) > 0) {
							plk_erase(it);
							--svc->waitSendMsgNum;
							--tmpMsgNum;
						}
					}
					if (tmpMsgNum < 1) {
						break;
					}
				}
			}
			if ((!svcExitCalled && (inMsgLeft > 0 || tmMsgLeft > 0 || netMsgLeft > 0))
					|| svc->waitSendMsgNum > 0 ) {
				svcNoBiz = false;
			}
			if (svcNoBiz && svcExitCalled) { // clear all msg 
				while(mpsc_pop(svcMqIn, pMsg)) {
					proc_svc_msg_in(pMsg, svc, svcUd);
				}
				// clear all timer msg
				if(svc->mqTimer) {
					while (spsc_pop(svc->mqTimer, &tmMsg)) {}
				}
				// clear all net msg
				if(svc->mqNet) {
					while (mpsc_pop(svc->mqNet, &netMsg)) {}
				}
				// notify svc exit
				msg_gen_svc_exit(pMsg);
				svcUd->cbOnMsg(pMsg, svcUd->userData, nullptr);  
				svcUd->cbOnMsg = nullptr;
				svcUd->userData = nullptr;
				svcUd->cbDeMsg = nullptr;
				svcUd->cbOnTimerMsg = nullptr;
				svcUd->cbOnNetMsg = nullptr;
			}
			SVC_RT_END(svc);
			//
			if(svcNoBiz) { // change onTask to false
				assert(svc->onTask.exchange(false));
				if(!svcExitCalled && 
						(mpsc_size(svcMqIn) > 0 
						|| (svc->mqTimer && spsc_size(svc->mqTimer) > 0)
						|| (svc->mqNet && mpsc_size(svc->mqNet) > 0 ))) {
					 // still has biz to proc, re-add task
					bool preState = false;
					if (svc->onTask.compare_exchange_strong(preState, true)) {
						worker_add_task(pipes, svc, false, wkSelf);
					}
				}else { // not onTask, decr totalTask
					uint32_t leftTask = atomTotalTask.fetch_sub(1, std::memory_order_relaxed) - 1;
					//
					//printf("task out, svcIdx=%d, leftTask=%d\n", svc->svcIdx, leftTask);
					//
					if(svcExitCalled) { // svc exit, back slot
						svc_back(pipes, svcIdx);
					}
				}
			}
			else { // still has biz to proc, re-add task
				worker_add_task(pipes, svc, false, wkSelf);
			}
		}else { // no task found, check idle
			int32_t idleWorkerNum = pipes->idleWorkerNum.fetch_add(1) + 1;
			uint32_t totalTask = atomTotalTask.load();
			if (totalTask <= workerNum - idleWorkerNum) {
				/*
				printf("worker(%d) will idle, totalTask=%d, idleWorker=%d\n", 
					curWorkerIdx,
					totalTask,
					idleWorkerNum);  // debug
					*/
				std::unique_lock<std::mutex> lock(pipes->mtxNotifyWorker);
				pipes->condNotifyWorker.wait_for(lock, std::chrono::milliseconds(waitTmout));
				//printf("worker(%d) idle finish\n", curWorkerIdx);  // debug
			}
			pipes->idleWorkerNum.fetch_sub(1);
			//printf("dddd thIdx=%d, totalTask=%d, idelNum=%d\n", curWorkerIdx, totalTask, idleWorkerNum);
		}
	}
	// wait all extthread done
	pps_wait_ext_thread_done(pipes);
	//
	pipes->loopExitedNum.fetch_add(1);
	// wait all worker exit
	while(pipes->loopExitedNum.load() < totalLoopNum) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
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
		int32_t totalTask = pipes->totalTask.fetch_add(1) + 1;   
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

static int64_t calc_idlewait_tmout(uint32_t wkIdx, uint32_t workerNum) {
	const int64_t tmMin = 3000;
	const int64_t tmMax = 10000;
	const int64_t tmDlt = tmMax - tmMin;
	int64_t tm = tmMin + (wkIdx + 1)*tmDlt / workerNum;
	if (tm > tmMax) {
		tm = tmMax;
	}
	return tm;
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
	return 1;
}
int worker_deinit_main_thread(struct pps_worker* wk)
{
	mpmc_destroy(wk->queTask);
	pps_free(wk->tmpBuf.buf);
	return 1;	
}