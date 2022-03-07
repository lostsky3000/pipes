#ifndef PPS_SERVICE_H
#define PPS_SERVICE_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include "mq_mpsc.h"
#include "mq_mpmc.h"
#include "mq_spsc.h"
#include "pps_macro.h"
#include "pool_linked.h"
#include "pps_timer.h"
#include "pps_net.h"

struct pipes;
struct pps_msg_body;
struct pps_message;
struct pps_service;
struct pps_worker;
struct pps_timer;
struct net_msg;

typedef void(*CB_SVC_MSG)(struct pps_message*m, void* adapter, struct pps_service* s);
typedef int(*CB_SVC_DEMSG)(struct pps_message*m, void* adapter);
typedef void(*CB_TIMER_MSG)(struct timer_msg*m, void* adapter, struct pps_service* s);
typedef void(*CB_NET_MSG)(struct net_msg*m, void* adapter, struct pps_service* s, struct pps_worker* wk);

struct pps_service_ud
{
	CB_NET_MSG cbOnNetMsg;
	CB_TIMER_MSG cbOnTimerMsg;
	CB_SVC_MSG cbOnMsg;
	CB_SVC_DEMSG cbDeMsg;
	void* userData;
};

struct pps_service
{
	bool exitCalled;
	std::atomic<bool> onTask;
	std::atomic<int16_t> calledCnt;
	int32_t svcIdx;
	std::atomic<uint32_t> svcCnt;
	uint32_t svcCntLocal;
	std::atomic<int32_t> lastWorker;
	int32_t waitSendMsgNum;
	struct pipes* pipes;
	struct pps_timer* timer;
	struct mq_mpsc<struct pps_message>* mqIn;
	struct pool_linked<struct pps_message>* mqWaitSend;
	struct mq_spsc<struct timer_msg>* mqTimer;
	struct mq_mpsc<struct net_msg>* mqNet;
	struct pps_worker* curWorker;
	struct exclusive_svc_ctx* exclusiveCtx;
	// user logic
	struct pps_service_ud ud;
};

struct pps_service_mgr
{
	struct mq_mpmc<uint32_t>* quIdxFree;
	struct mq_mpmc<uint32_t>* quIdxOri;
	struct mq_mpmc<uint32_t>* quIdxFreeExclusive;
	struct mq_mpmc<uint32_t>* quIdxOriExclusive;
	struct pps_service slots[SVC_SLOT_NUM];
};

// service
static inline void svc_runtime_begin(struct pps_service* s) {
	s->svcCnt.load(std::memory_order_acquire);
}
static inline void svc_runtime_end(struct pps_service* s) {
	s->svcCnt.fetch_add(0, std::memory_order_release);
}
#define SVC_RT_BEGIN(pS_) svc_runtime_begin(pS_);
#define SVC_RT_END(pS_) svc_runtime_end(pS_);

int32_t svc_newservice(struct pipes* pipes, struct pps_service_ud* ud, 
	uint32_t* outSvcCnt, struct pps_service* caller, int32_t mqInCap, bool exclusive);

int32_t svc_sendmsg(uint32_t idxTo, struct pps_message* msg, struct pipes* pipes, struct pps_service* caller);

int32_t svc_send_timermsg(uint32_t idxTo, struct timer_msg* msg, struct pps_timer* timer);
int32_t svc_send_netmsg(uint32_t idxTo, struct net_msg_ori* msg, struct pipes* pipes);

inline int svc_check_mqtimer_init(struct pps_service* s)
{
	if (s->mqTimer == nullptr) {
		s->mqTimer = spsc_create<struct timer_msg>(SVC_MQTIMER_LEN);
		spsc_read_thread_acquire(s->mqTimer);
		SVC_RT_END(s);   // ensure mq visible
		return 1;
	}
	return 0;
}
inline int svc_check_mqnet_init(struct pps_service* s)
{
	if(s->mqNet == nullptr) {
		s->mqNet = const_cast<struct mq_mpsc<struct net_msg>*>(mpsc_create<struct net_msg>(SVC_MQNET_LEN));
		SVC_RT_END(s);    // ensure mq visible
		return 1;
	}
	return 0;
}

inline int32_t svc_exit(struct pps_service* caller)
{
	if (caller->exitCalled) {
		return 0;
	}
	caller->exitCalled = true;
	caller->svcCntLocal = caller->svcCnt.fetch_add(1) + 1;
	return 1;
}
int32_t svc_back(struct pipes* pipes, uint32_t svcIdx);
// service mgr
struct pps_service_mgr;
const struct pps_service_mgr* svcmgr_create(struct pipes* pipes);
void svcmgr_destroy(const struct pps_service_mgr* mgr);


inline struct pps_service* svcmgr_get_svc(struct pps_service_mgr* mgr, uint32_t svcIdx)
{
	if (svcIdx <= SVC_SLOT_NUM) {
		return &mgr->slots[svcIdx];
	}
	return nullptr;
}

//
struct exclusive_svc_ctx
{
	std::atomic<bool> isIdle;
	std::atomic<uint32_t> msgNum;
	std::mutex mtx;
	std::condition_variable cond;
};
struct exclusive_thread_ctx
{
	std::atomic<bool> loop;
	struct pipes* pipes;
	struct pps_worker* worker;
	struct pps_service* svc;
	struct exclusive_svc_ctx* svcCtx;
};
struct pps_exclusive_mgr
{
	bool shutdownCalled;
	struct pipes* pipes;
	std::mutex mtx;
	struct pool_linked<struct exclusive_thread_ctx*>* plkCtx;
};
int exclusive_mgr_init(struct pps_exclusive_mgr* mgr, struct pipes* pipes);
void exclusive_mgr_deinit(struct pps_exclusive_mgr* mgr);
int exclusive_mgr_newsvc(struct pps_service* s, struct pps_exclusive_mgr* mgr);
int exclusive_mgr_svcexit(struct exclusive_thread_ctx* ctx);
void exclusive_mgr_waitalldone(struct pps_exclusive_mgr* mgr);
void exclusive_mgr_shutdown(struct pps_exclusive_mgr* mgr);

#endif // !PPS_SERVICE_H

