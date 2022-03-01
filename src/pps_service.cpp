
#include "pps_service.h"
#include "pipes.h"
#include "pps_message.h"
#include "pps_worker.h"
#include "pps_config.h"


static struct pps_service* alloc_service_slot(struct pps_service_mgr* mgr);

// service 
int32_t svc_newservice(struct pipes* pipes, struct pps_service_ud* ud, 
	uint32_t* outSvcCnt, struct pps_service* caller, int32_t mqInCap)
{
	if (caller && caller->exitCalled) {  // has caller & caller exit
		return -1;
	}
	if (pipes->totalService.load(std::memory_order_relaxed) >= MAX_SERVICE) {
		return -2;
	}
	if (mqInCap == 0) {  // not specify mqInCap, use default
		mqInCap = SVC_MQIN_CAP_DEFAULT;
	}
	else if (mqInCap < 0 || !mpsc_check_capacity<struct pps_message>(mqInCap)) {
		return -3;   // specify invliad mqInLen
	}
	struct pps_service* s = alloc_service_slot(pipes->serviceMgr);
	if (s == nullptr) {  // no more service slot
		return -4;
	}
	pipes->totalService.fetch_add(1, std::memory_order_relaxed);
	SVC_RT_BEGIN(s);
	s->ud = *ud;
	if (s->mqIn) { // mqIn exist,compare cap
		uint32_t capOld = mpsc_capacity(s->mqIn);
		if (capOld != mqInCap) {   // cap not match, re-alloc
			mpsc_destroy(s->mqIn);
			s->mqIn = nullptr;
		}
	}
	if (s->mqIn == nullptr) {  // need create mqIn
		s->mqIn = const_cast<struct mq_mpsc<struct pps_message>*>(mpsc_create<struct pps_message>(mqInCap));
	}
	s->exitCalled = false;
	s->exited = false;
	s->svcCntLocal = s->svcCnt.load(std::memory_order_relaxed);
	// alloc timer
	s->timer = &pipes->timers[pipes->timerAllocCnt.fetch_add(1, std::memory_order_relaxed) % pipes->config->timer_num];
	SVC_RT_END(s);
	*outSvcCnt = s->svcCntLocal;
	// send start msg
	struct pps_message msg;
	msg.from_cnt = 0;
	msg.to_cnt = s->svcCntLocal;
	msg.idx_pair = idxpair_encode(0, s->svcIdx);
	msg.session = 0;
	msg.size = msg_size_combine(MTYPE_USER_START, 0);
	msg.data = nullptr;
	assert(svc_sendmsg(s->svcIdx, &msg, pipes, caller) > 0);
	return s->svcIdx;
}

static inline int add_to_waitsend(struct pps_service*s, struct pps_message* m) {
	if (s->waitSendMsgNum >= INT32_MAX) {
		return 0;
	}
	if (s->mqWaitSend == nullptr) {
		s->mqWaitSend = const_cast<struct pool_linked<struct pps_message>*>(plk_create<struct pps_message>(SVC_MQ_WAITSEND_CAP));
	}
	plk_push_ptr(s->mqWaitSend, m);
	++s->waitSendMsgNum;
	return 1;
}

int32_t svc_sendmsg(uint32_t idxTo, struct pps_message* msg, struct pipes* pipes, struct pps_service* caller)
{
	if (caller && caller->exitCalled && msg->session > 0) {   // has caller & caller has exit & req of call
		return -1;
	}
	struct pps_service* s = svcmgr_get_svc(pipes->serviceMgr, idxTo);
	if (s == nullptr) {   // idxTo invalid
		return -2;
	}
	if (msg->to_cnt != s->svcCnt.load(std::memory_order_relaxed)) {  // svcCnt not match, svc may gone
		return -3;
	}
	SVC_RT_BEGIN(s);	// ensure mqIn not null
	if (caller) {    // has caller, check mqWaitSend
		if(caller->waitSendMsgNum > 0 || !mpsc_push_ptr(s->mqIn, msg)) { // has waitSendMsg
		    if(!add_to_waitsend(caller, msg)) { // mqWaitSend is full !!! 
			   return 0;
			}
		}
	}else if (!mpsc_push_ptr(s->mqIn, msg)) {  // mqIn is full
		return 0;
	}
	// add task to worker
	bool preState = false;
	if (s->onTask.compare_exchange_strong(preState, true)) {
		worker_add_task(pipes, s, true);
	}
	return 1;
}

int32_t svc_send_timermsg(uint32_t idxTo, struct timer_msg* msg, struct pps_timer* timer)
{
	struct pipes* pipes = timer->pipes;
	struct pps_service* s = svcmgr_get_svc(pipes->serviceMgr, idxTo);
	if (msg->svcCnt != s->svcCnt.load(std::memory_order_relaxed)) { // svcCnt not match, svc may gone
		return -1;
	}
	if (s->mqTimer == nullptr) {
		SVC_RT_BEGIN(s);    // ensure mqTimer visible
	}
	if(!s->mqVisible4Timer[timer->index]){
		spsc_write_thread_init(s->mqTimer);
		s->mqVisible4Timer[timer->index] = true;
	}
	if (!spsc_push_ptr(s->mqTimer, msg)) { // mq is full, immposible
		assert(false);  // debug
		//return 0;
	}
	// add task to worker
	bool preState = false;
	if (s->onTask.compare_exchange_strong(preState, true)) {
		worker_add_task(pipes, s, true);
	}
	return 1;
}

int32_t svc_send_netmsg(uint32_t idxTo, struct net_msg_ori* msg, struct pipes* pipes)
{
	struct pps_service* s = svcmgr_get_svc(pipes->serviceMgr, idxTo);
	if (msg->to.cnt != s->svcCnt.load(std::memory_order_relaxed)) {
		 // svcCnt not match, svc may gone
		return - 1;
	}
	if (s->mqNet == nullptr) {
		SVC_RT_BEGIN(s);     // ensure mqNet visible
	}
	if(!mpsc_push_ptr_custom(s->mqNet, msg, net_msg_push_fill)) {   // mq is full
		return 0;
	}
	// add task to worker
	bool preState = false;
	if (s->onTask.compare_exchange_strong(preState, true)) {
		worker_add_task(pipes, s, true);
	}
	return 1;
}

int32_t svc_back(struct pipes* pipes, uint32_t svcIdx)
{
	assert(mpmc_push(pipes->serviceMgr->quIdxFree, svcIdx));
	pipes->totalService.fetch_sub(1, std::memory_order_relaxed);	
	return 1;	
}

// service mgr
static struct pps_service* alloc_service_slot(struct pps_service_mgr* mgr)
{
	uint32_t svcIdx = -1;
	if (mpmc_pop(mgr->quIdxFree, &svcIdx)
		|| mpmc_pop(mgr->quIdxOri, &svcIdx)) {  // got from freeQueue or oriQueue
		return &mgr->slots[svcIdx];
	}
	// no more slot
	return nullptr;
}

const struct pps_service_mgr* svcmgr_create(struct pipes* pipes)
{
	struct pps_config* cfg = pipes->config;
	struct pps_service_mgr* mgr = new struct pps_service_mgr;
	struct pps_service* arrSvc = mgr->slots;
	//
	mgr->quIdxFree = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(SVC_SLOT_NUM));
	mgr->quIdxOri = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(SVC_SLOT_NUM));
	// init arrSvc
	for(uint32_t i = 0 ; i < SVC_SLOT_NUM ; ++i) {
		struct pps_service& svc = arrSvc[i];
		svc.svcIdx = i;
		svc.pipes = pipes;
		svc.mqIn = nullptr;
		svc.mqWaitSend = nullptr;
		svc.mqTimer = nullptr;
		svc.mqVisible4Timer = new bool[cfg->timer_num];
		for(int j=0; j<cfg->timer_num; ++j){
			svc.mqVisible4Timer[j] = false;
		}
		svc.mqNet = nullptr;
		svc.waitSendMsgNum = 0;
		svc.onTask.store(false);
		svc.lastWorker.store(-1);
		svc.svcCnt.store(UINT32_MAX / 2 + i, std::memory_order_release);   //init cnt with a largeNum
		//
		assert(mpmc_push(mgr->quIdxOri, i));
	}
	return mgr;
}
void svcmgr_destroy(const struct pps_service_mgr* mgr)
{
	struct pps_service_mgr* mg = const_cast<struct pps_service_mgr*>(mgr);
	//
	mpmc_destroy(mgr->quIdxOri);
	mpmc_destroy(mgr->quIdxFree);
	delete mgr;
}




