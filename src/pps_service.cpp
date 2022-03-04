
#include "pps_service.h"
#include "pipes.h"
#include "pps_message.h"
#include "pps_worker.h"
#include "pps_config.h"


static struct pps_service* alloc_service_slot(struct pps_service_mgr* mgr);
static struct pps_service* alloc_exclusive_slot(struct pps_service_mgr* mgr);
static inline int add_to_waitsend(struct pps_service*s, struct pps_message* m);
static inline bool is_svc_exclusive(uint32_t svcIdx)
{
	return svcIdx < MAX_EXCLUSIVE;
}
// service 
int32_t svc_newservice(struct pipes* pipes, struct pps_service_ud* ud, 
	uint32_t* outSvcCnt, struct pps_service* caller, int32_t mqInCap, bool exclusive)
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
	struct pps_service* s = nullptr;
	if(exclusive){
		s = alloc_exclusive_slot(pipes->serviceMgr);
	}else{
		s = alloc_service_slot(pipes->serviceMgr);
	}
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
	s->svcCntLocal = s->svcCnt.load(std::memory_order_relaxed);
	// alloc timer
	s->timer = &pipes->timers[pipes->timerAllocCnt.fetch_add(1, std::memory_order_relaxed) % pipes->config->timer_num];
	SVC_RT_END(s);
	*outSvcCnt = s->svcCntLocal;
	//
	if (exclusive) {
		exclusive_mgr_newsvc(s, pipes->exclusiveMgr);
	}
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
inline void callsvc_begin(struct pps_service* s)
{
	s->calledCnt.fetch_add(1);
}
inline void callsvc_end(struct pps_service* s)
{
	s->calledCnt.fetch_sub(1);
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
	callsvc_begin(s);
	if (msg->to_cnt != s->svcCnt.load()) {  // svcCnt not match, svc may gone
		callsvc_end(s);
		return -3;
	}
	//SVC_RT_BEGIN(s);	// ensure mqIn not null
	if (caller) {    // has caller, check mqWaitSend
		if(caller->waitSendMsgNum > 0 || !mpsc_push_ptr(s->mqIn, msg)) { // has waitSendMsg
			callsvc_end(s);
		    if(!add_to_waitsend(caller, msg)) { // mqWaitSend is full !!! 	
			    return 0;
			}
			return 1;
		}
	}else if (!mpsc_push_ptr(s->mqIn, msg)) {  // mqIn is full
		callsvc_end(s);
		return 0;
	}
	if(is_svc_exclusive(idxTo)){   // dst is exclusive
		struct exclusive_svc_ctx* exclusiveCtx = s->exclusiveCtx;
		exclusiveCtx->msgNum.fetch_add(1);
		if(exclusiveCtx->isIdle.load()){
			std::unique_lock<std::mutex> lock(exclusiveCtx->mtx);
			exclusiveCtx->cond.notify_all();
		}
	}else {
		// add task to worker
		bool preState = false;
		if (s->onTask.compare_exchange_strong(preState, true)) {
			worker_add_task(pipes, s, true);
		}
	}
	callsvc_end(s);
	return 1;
}

int32_t svc_send_timermsg(uint32_t idxTo, struct timer_msg* msg, struct pps_timer* timer)
{
	struct pipes* pipes = timer->pipes;
	struct pps_service* s = svcmgr_get_svc(pipes->serviceMgr, idxTo);
	callsvc_begin(s);
	if (msg->svcCnt != s->svcCnt.load()) { // svcCnt not match, svc may gone
		callsvc_end(s);
		return -1;
	}
	/*
	if (s->mqTimer == nullptr) {
		SVC_RT_BEGIN(s);    // ensure mqTimer visible
	}*/
	if(!s->mqVisible4Timer[timer->index]){
		spsc_write_thread_init(s->mqTimer);
		s->mqVisible4Timer[timer->index] = true;
	}
	if (!spsc_push_ptr(s->mqTimer, msg)) { // mq is full, imposible
		assert(false);  // debug
		//return 0;
	}
	if(is_svc_exclusive(idxTo)){  // dst is exclusive
		struct exclusive_svc_ctx* exclusiveCtx = s->exclusiveCtx;
		exclusiveCtx->msgNum.fetch_add(1);
		if (exclusiveCtx->isIdle.load()) {
			std::unique_lock<std::mutex> lock(exclusiveCtx->mtx);
			exclusiveCtx->cond.notify_all();
		}
	}else{
		// add task to worker
		bool preState = false;
		if (s->onTask.compare_exchange_strong(preState, true)) {
			worker_add_task(pipes, s, true);
		}
	}
	callsvc_end(s);
	return 1;
}

int32_t svc_send_netmsg(uint32_t idxTo, struct net_msg_ori* msg, struct pipes* pipes)
{
	struct pps_service* s = svcmgr_get_svc(pipes->serviceMgr, idxTo);
	callsvc_begin(s);
	if (msg->to.cnt != s->svcCnt.load()) {  // svcCnt not match, svc may gone
		callsvc_end(s);
		return - 1;
	}
	/*
	if (s->mqNet == nullptr) {
		SVC_RT_BEGIN(s);     // ensure mqNet visible
	}
	*/
	if(!mpsc_push_ptr_custom(s->mqNet, msg, net_msg_push_fill)) {   // mq is full
		callsvc_end(s);
		return 0;
	}
	if(is_svc_exclusive(idxTo)){   // dst is exclusive
		struct exclusive_svc_ctx* exclusiveCtx = s->exclusiveCtx;
		exclusiveCtx->msgNum.fetch_add(1);
		if (exclusiveCtx->isIdle.load()) {
			std::unique_lock<std::mutex> lock(exclusiveCtx->mtx);
			exclusiveCtx->cond.notify_all();
		}
	}else{
		// add task to worker
		bool preState = false;
		if (s->onTask.compare_exchange_strong(preState, true)) {
			worker_add_task(pipes, s, true);
		}
	}
	callsvc_end(s);
	return 1;
}

int32_t svc_back(struct pipes* pipes, uint32_t svcIdx)
{
	if(is_svc_exclusive(svcIdx)){
		if(!mpmc_push(pipes->serviceMgr->quIdxFreeExclusive, svcIdx)){
			while(!mpmc_push(pipes->serviceMgr->quIdxFreeExclusive, svcIdx)){}
		}
	}else{
		assert(mpmc_push(pipes->serviceMgr->quIdxFree, svcIdx));
	}
	pipes->totalService.fetch_sub(1, std::memory_order_relaxed);	
	return 1;	
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
static struct pps_service* alloc_exclusive_slot(struct pps_service_mgr* mgr)
{
	uint32_t svcIdx = -1;
	if (mpmc_pop(mgr->quIdxFreeExclusive, &svcIdx)
		|| mpmc_pop(mgr->quIdxOriExclusive, &svcIdx)) {  // got from freeQueue or oriQueue
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
	mgr->quIdxFreeExclusive = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(MAX_EXCLUSIVE));
	mgr->quIdxOriExclusive = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(MAX_EXCLUSIVE));
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
		svc.calledCnt.store(0);
		svc.exclusiveCtx = nullptr;
		svc.svcCnt.store(UINT32_MAX / 2 + i, std::memory_order_release);   //init cnt with a largeNum
		if(i >= MAX_EXCLUSIVE){
			assert(mpmc_push(mgr->quIdxOri, i));
		}else{
			assert(mpmc_push(mgr->quIdxOriExclusive, i));
		}
	}
	return mgr;
}
void svcmgr_destroy(const struct pps_service_mgr* mgr)
{
	mpmc_destroy(mgr->quIdxOri);
	mpmc_destroy(mgr->quIdxFree);
	mpmc_destroy(mgr->quIdxOriExclusive);
	mpmc_destroy(mgr->quIdxFreeExclusive);
	delete mgr;
}


//
int exclusive_mgr_newsvc(struct pps_service* s, struct pps_exclusive_mgr* mgr)
{
	// init ctx
	struct exclusive_svc_ctx* ctxSvc = new struct exclusive_svc_ctx;
	ctxSvc->isIdle.store(false);
	ctxSvc->msgNum.store(0);
	s->exclusiveCtx = ctxSvc;
	SVC_RT_END(s);
	struct exclusive_thread_ctx* ctxThread = new struct exclusive_thread_ctx;
	ctxThread->pipes = mgr->pipes;
	ctxThread->svc = s;
	ctxThread->worker = new pps_worker;
	ctxThread->svcCtx = ctxSvc;
	ctxThread->loop.store(true);
	// start thread
	std::thread th = std::thread(worker_exclusive_thread, ctxThread);
	th.detach();
	// 
	{
		std::lock_guard<std::mutex> lock(mgr->mtx);
		plk_push(mgr->plkCtx, ctxThread);
	}
	return 1;
}
int exclusive_mgr_svcexit(struct exclusive_thread_ctx* ctx)
{
	struct pps_exclusive_mgr* mgr = ctx->pipes->exclusiveMgr;
	//
	std::lock_guard<std::mutex> lock(mgr->mtx);
	const plk_iterator<struct exclusive_thread_ctx*> it = plk_it_init(mgr->plkCtx);
	struct exclusive_thread_ctx* pCtx;
	for (; plk_it_loop(it);) {
		pCtx = **it;
		if(pCtx == ctx){
			plk_erase(it);
			return 1;
		}
	}	
	return 0;
}
void exclusive_mgr_waitalldone(struct pps_exclusive_mgr* mgr)
{
	while(true){
		std::lock_guard<std::mutex> lock(mgr->mtx);
		if(plk_size(mgr->plkCtx) <= 0){
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}
void exclusive_mgr_shutdown(struct pps_exclusive_mgr* mgr)
{
	std::lock_guard<std::mutex> lock(mgr->mtx);
	if(mgr->shutdownCalled){
		return;
	}
	mgr->shutdownCalled = true;
	if(plk_size(mgr->plkCtx) > 0){
		const plk_iterator<struct exclusive_thread_ctx*> it = plk_it_init(mgr->plkCtx);
		struct exclusive_thread_ctx* ctx;
		for (; plk_it_loop(it);) {
			ctx = **it;
			ctx->loop.store(false);
			std::unique_lock<std::mutex> lockLoop(ctx->svcCtx->mtx);
			ctx->svcCtx->cond.notify_all();
		}
	}
}
int exclusive_mgr_init(struct pps_exclusive_mgr* mgr, struct pipes* pipes)
{
	mgr->pipes = pipes;
	std::lock_guard<std::mutex> lock(mgr->mtx);
	mgr->plkCtx = const_cast<struct pool_linked<struct exclusive_thread_ctx*>*>(plk_create<struct exclusive_thread_ctx*>(128));
	mgr->shutdownCalled = false;
	return 1;
}
void exclusive_mgr_deinit(struct pps_exclusive_mgr* mgr)
{
	std::lock_guard<std::mutex> lock(mgr->mtx);
	plk_destroy(mgr->plkCtx);
}




