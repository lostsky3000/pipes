#ifndef PIPES_H
#define PIPES_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "pps_config.h"

struct pipes;
struct pps_logger;

typedef int(*FN_PPS_BOOT_CB)(void* ud, struct pipes* pipes);

struct pps_worker;
struct pps_timer;
struct pps_net;
struct pps_service_mgr;
struct socket_mgr;
struct timer_clock_point;
struct pps_exclusive_mgr;

struct pps_boot_ud
{
	FN_PPS_BOOT_CB cb;
	void* ud;
};
struct share_table_mgr;
struct pipes
{
	std::atomic<bool> hasShutdown;
	std::atomic<uint32_t> svcWorkerCnt;
	std::atomic<uint32_t> totalWorkerTask;
	std::atomic<int32_t> idleWorkerNum;
	std::atomic<int32_t> totalService;
	//
	std::atomic<uint32_t> totalLoopThread;
	std::atomic<uint32_t> loopStartedNum;
	std::atomic<uint32_t> loopExitedNum;
	std::atomic<uint32_t> netAllocCnt;
	std::atomic<uint32_t> timerAllocCnt;
	std::atomic<uint32_t> extThreadNum;
	//
	struct pps_config* config;
	struct pps_worker* workers;
	struct pps_timer* timers;
	struct pps_net* nets;
	struct pps_logger* logger;
	struct pps_service_mgr* serviceMgr;
	struct socket_mgr* sockMgr;
	struct pps_exclusive_mgr* exclusiveMgr;
	struct share_table_mgr* shareTableMgr;
	//
	struct timer_clock_point* tmStart;
	//
	std::mutex mtxNotifyWorker;
	std::condition_variable condNotifyWorker;
	//
	struct pps_boot_ud ud;
	//
	pipes()
		: svcWorkerCnt(0)
		, totalWorkerTask(0)
		, idleWorkerNum(0)
		, totalLoopThread(0)
		, loopStartedNum(0)
		, loopExitedNum(0)
		, hasShutdown(false)
		, totalService(0)
		, extThreadNum(0)
		
	{
		
	}
};

int pps_shutdown(struct pipes* pipes);

inline bool pps_hasnet(struct pipes* pipes)
{
	return pipes->config->net_num > 0;	
}


typedef void(*FN_EXT_THREAD)(struct pipes* pipes, void*ud);
int pps_ext_thread(struct pipes* pipes, FN_EXT_THREAD cb, void* ud);
void pps_wait_ext_thread_done(struct pipes* pipes);


#endif // !PIPES_H



