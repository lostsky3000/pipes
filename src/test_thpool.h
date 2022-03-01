#ifndef TEST_THPOOL_H
#define TEST_THPOOL_H

#include "thread_pool.h"
#include <atomic>
#include "pps_sysinfo.h"

const int g_thp_worker_num = 6;
const int32_t g_thp_totalTask = 1000000;
const int32_t g_thp_shutdownCnt = 10;
std::atomic<int32_t> g_thp_taskSendCnt(g_thp_totalTask);
std::atomic<int32_t> g_thp_taskReadCnt(g_thp_totalTask);
struct thread_pool* g_pool = nullptr;

struct st_thp_test_ud
{
	int32_t num;
};
static bool on_tick(void* ud)
{
	if (ud == nullptr) {
		int n = 1;
	}
	struct st_thp_test_ud* data = (struct st_thp_test_ud*)ud;
	
	//delete data;
	g_thp_taskReadCnt.fetch_sub(1);
	return true;	
}
static void ud_free(void* ud) {
	struct st_thp_test_ud* data = (struct st_thp_test_ud*)ud;
	delete data;
}
static void fn_worker()
{
	while (true) {
		if (thpool_is_shutdown(g_pool)) {
			break;
		}
		int32_t sendCnt = g_thp_taskSendCnt.fetch_sub(1);
		if (sendCnt <= 0) {
			break;
		}
		if (sendCnt % 10000 == 0) {  // debug
			printf(">>>>>>>>>>>> test sendCnt, now=%d\n", sendCnt);
		}
		struct st_thp_test_ud* ud = new struct st_thp_test_ud;
		ud->num = sendCnt;
		int32_t loopCnt = 0;
		int32_t err = 0;
		while (!thpool_post_task(g_pool, ud, on_tick, ud_free, &err))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(30)); 
			/**/
			if (++loopCnt % 200 == 0) { // debug
				loopCnt = 0;
				printf(">>>>>>>>>>>> test sendBlock, sendCnt=%d, err=%d, usedThNum=%d, idleThNum=%d, taskNum=%d\n",
					sendCnt,
					err,
					g_pool->used_thread_num.load(),
					g_pool->idle_thread_num.load(),
					g_pool->task_num.load());
				
			}
			if (thpool_is_shutdown(g_pool)) {
				delete ud;
				break;
			}
		}
		//
		if(g_thp_shutdownCnt == sendCnt) {  // shutdown
			printf(">>>>>>>>>>>> call thpool shutdown\n");
			thpool_shutdown_sync(g_pool);
		}
	}
}
void test_thpool()	
{
	uint32_t coreNum = sysinfo_core_num();
	struct thread_pool_cfg cfg;
	cfg.thread_max = thpool_thread_limit();
	cfg.task_max = thpool_task_limit();
	cfg.idle_min = 3;
	cfg.idle_max = 7;
	//
	g_pool = const_cast<struct thread_pool*>(thpool_create(&cfg));
	// start worker thread
	std::thread arrTh[128];
	for (int i = 0; i < g_thp_worker_num; ++i) {
		arrTh[i] = std::thread(fn_worker);
	}
	for (int i = 0; i < g_thp_worker_num; ++i) {
		arrTh[i].join();
	}
	//
	while(!thpool_is_shutdown(g_pool)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	thpool_destroy(g_pool);
}

#endif // !TEST_THPOOL_H

