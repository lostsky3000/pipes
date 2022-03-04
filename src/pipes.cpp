#define LUA_LIB

#include "pipes.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"

LUAMOD_API int luaopen_pipesc(lua_State *L);
LUAMOD_API int luaopen_pipesc_boot(lua_State *L);
}
#include "lpps_adapter.h"

#include <thread>
#include <mutex>
#include <condition_variable>


#include "pps_worker.h"
#include "pps_service.h"
#include "pps_macro.h"
#include "pps_logger.h"
#include "pps_timer.h"
#include "pps_net.h"
#include "pps_socket.h"

static int pipes_init(struct pipes* pipes);
static int pipes_run(struct pipes* pipes);

static int l_test(lua_State* L) 
{
	return 0;
}

LUAMOD_API int
luaopen_pipesc(lua_State *L) 
{
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "test", l_test },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}

static int
l_init(lua_State* L) 
{
	if (lua_getfield(L, LUA_REGISTRYINDEX, LPPS_PIPES_NAME) != LUA_TNIL) 
	{
		lua_pop(L, 1);
		return luaL_error(L, "already init");
	}
	lua_pop(L, 1);
	struct pps_config* config = config_init(L);
	struct pipes* pipes = new struct pipes;// (struct pipes*)pps_malloc(sizeof(*pipes));
	pipes->config = config;
	int ret = pipes_init(pipes);
	if (ret) {  // init pipes error
		return luaL_error(L, "init pipes failed: %d", ret);
	}
	lua_pushlightuserdata(L, pipes);
	lua_setfield(L, LUA_REGISTRYINDEX, LPPS_PIPES_NAME);

	return 0;
}

static struct pipes* get_pipes(lua_State* L) 
{
	struct pipes* pipes = NULL;
	int type = lua_getfield(L, LUA_REGISTRYINDEX, LPPS_PIPES_NAME);
	if (type == LUA_TLIGHTUSERDATA)
	{
		pipes = (struct pipes*)lua_touserdata(L, -1);
	}
	lua_pop(L, 1);
	return pipes;
}
static int 
l_deinit(lua_State* L) 
{
	//return 0;  // debug
	struct pipes* pipes = get_pipes(L);
	//
	if(pipes) {
		// destroy timers
		for(uint32_t i=0; i<pipes->config->timer_num; ++i) {
			timer_deinit_main_thread(&pipes->timers[i]);		
		}
		delete[] pipes->timers;
		// destroy workers
		for (uint32_t i=0; i<pipes->config->worker_num; ++i) {
			worker_deinit_main_thread(&pipes->workers[i]);
		}
		delete[] pipes->workers;
		
		// destroy sockMgr & nets
		if(pipes->config->net_num > 0) {
			struct socket_mgr* sockMgr = pipes->nets[0].sockMgr;
			for(uint32_t i = 0 ; i < pipes->config->net_num ; ++i) {
				net_deinit_main_thread(&pipes->nets[i]);
			}
			delete[] pipes->nets;
			//
			sockmgr_deinit_main_thread(sockMgr);
			delete sockMgr;
		}
		// destroy exclusive-mgr
		exclusive_mgr_deinit(pipes->exclusiveMgr);
		// destroy service-mgr
		svcmgr_destroy(pipes->serviceMgr);
		// destroy logger
		logger_deinit_main_thread(pipes->logger);
		delete pipes->logger;
		// destroy tmStart
		delete pipes->tmStart;
		// destroy config
		config_deinit(pipes->config);
		// destroy pipes
		delete pipes;
	}
	return 0;
}

static int pipes_init(struct pipes* pipes) 
{
	int ret = -1;
	do {
		struct pps_config* cfg = pipes->config;
		// init tmStart
		pipes->tmStart = new timer_clock_point;
		// init service-mgr
		pipes->serviceMgr = const_cast<struct pps_service_mgr*>(svcmgr_create(pipes));
		// init exclusive-mgr
		pipes->exclusiveMgr = new struct pps_exclusive_mgr;
		assert(exclusive_mgr_init(pipes->exclusiveMgr, pipes));
		// init logger
		pipes->logger = new struct pps_logger;
		logger_init_main_thread(pipes->logger, pipes);
		// init timers
		pipes->timers = new struct pps_timer[cfg->timer_num];
		for (uint32_t i=0; i<cfg->timer_num; ++i) {
			timer_init_main_thread(&pipes->timers[i], pipes, i);
		}
		// init workers
		pipes->workers = new struct pps_worker[cfg->worker_num];
		for (uint32_t i=0; i<cfg->worker_num; ++i) {
			worker_init_main_thread(&pipes->workers[i], pipes, i);
		}
		if (cfg->net_num > 0) { // specify net
			// init sockCtxMgr
			struct socket_mgr* sockMgr = new struct socket_mgr;
			sockmgr_init_main_thread(sockMgr, pipes);
			// init nets
			pipes->nets = new struct pps_net[cfg->net_num];
			for (uint32_t i = 0; i < cfg->net_num; ++i) {
				int netRet = net_init_main_thread(&pipes->nets[i], pipes, sockMgr, i);
				if(!netRet){    // init net failed
					return -2;
				}
			}
			pipes->sockMgr = sockMgr;
		}
		else {
			pipes->nets = nullptr;
			pipes->sockMgr = nullptr;
		}
		pipes->netAllocCnt.store(0);
		pipes->timerAllocCnt.store(0);
		// reg boot cb
		pipes->ud.cb = lpps_onboot;
		pipes->ud.ud = nullptr;
		ret = 0;
	} while (false);
	return ret;
}

#include "test_portal.h"
static int 
l_run(lua_State* L) 
{	
	// test 
	//test_portal_start();
	//
	struct pipes* pipes = get_pipes(L);
	if (pipes == NULL) {
		return luaL_error(L, "no pipes found");
	}
	int ret = pipes_run(pipes);
	if (ret) {  // run error
		return luaL_error(L, "pipes run error: %d", ret);
	}
	return 0;
}

LUAMOD_API int
luaopen_pipesc_boot(lua_State *L) 
{
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "init", l_init },
		{ "deinit", l_deinit },
		{ "run", l_run},
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}

static int pipes_run(struct pipes* pipes)
{
	using namespace std;
	int ret = -1;
	do {
		struct pps_config* cfg = pipes->config;
		// ret tmStart
		timer_clock_now(pipes->tmStart);
		//
		uint32_t totalLoop = cfg->worker_num + cfg->timer_num 
			+ cfg->net_num + 1; // worker + timer + net + logger
		pipes->totalLoopThread.store(totalLoop);
		// start all loops
		uint32_t normalThNum = cfg->worker_num + cfg->net_num + 1;  // worker + net + logger
		thread arrThNormal[MAX_WORKER + MAX_NET + 1]; 
		for (uint32_t i = 0; i < normalThNum; ++i) {
			if (i < cfg->worker_num) {   // worker thread
			   arrThNormal[i] = thread(worker_thread, &pipes->workers[i]);
			}
			else if (i < cfg->worker_num + cfg->net_num) {  // net thread
				arrThNormal[i] = thread(net_thread, &pipes->nets[i - cfg->worker_num]);
			}
			else {  // logger thread
				arrThNormal[i] = thread(logger_thread, pipes->logger);
			}
		}
		//
		uint32_t prioThNum = cfg->timer_num;  // timer
#if defined(USE_LINUX_TIMER)
		pthread_t arrThPrio[MAX_TIMER];
		pthread_attr_t arrThAttr[MAX_TIMER];
		struct sched_param arrThOpt[MAX_TIMER];
		int policy = SCHED_RR;
		int prioMax = sched_get_priority_max(policy);
		int prioMin = sched_get_priority_min(policy);
		int priority = (prioMin + prioMax) / 2;
		priority = priority < 10 ? 10 : priority;
		for (uint32_t i = 0; i < prioThNum; ++i) { // timer thread
			arrThOpt[i].__sched_priority = priority;
			pthread_attr_t* pAttr = &arrThAttr[i];
			pthread_attr_init(pAttr);
			assert(pthread_attr_setinheritsched(pAttr, PTHREAD_EXPLICIT_SCHED) == 0);
			assert(pthread_attr_setschedpolicy(pAttr, policy) == 0);
			assert(pthread_attr_setschedparam(pAttr, &arrThOpt[i]) == 0);
			pthread_create(&arrThPrio[i],
				pAttr,
				timer_thread,
				(void*)&pipes->timers[i]);
		}
#else
		thread arrThPrio[MAX_TIMER];
		for (uint32_t i = 0; i < prioThNum; ++i) {  // timer thread
			arrThPrio[i] = thread(timer_thread, &pipes->timers[i]);
		}
#endif
		for (uint32_t i = 0; i < normalThNum; ++i) {
			arrThNormal[i].join();
		}
#if defined(USE_LINUX_TIMER)
		for (uint32_t i = 0; i < prioThNum; ++i) {
			pthread_join(arrThPrio[i], NULL);
		}
		for (uint32_t i = 0; i < prioThNum; ++i) {
			pthread_attr_destroy(&arrThAttr[i]);
		}
#else
		for (uint32_t i = 0; i < prioThNum; ++i) {
			arrThPrio[i].join();
		}
#endif
		
		ret = 0;
	} while (false);
	return ret;
}

int pps_shutdown(struct pipes* pipes)
{
	bool preState = false;
	if (!pipes->hasShutdown.compare_exchange_strong(preState, true)) {  // dup called shutdown
		return 0;
	}
	// stop all exclusive event loop
	exclusive_mgr_shutdown(pipes->exclusiveMgr);
	// stop all workers event loop
	worker_shutdown(pipes);
	// stop all timer event loop
	timer_shutdown(pipes);
	// stop all net event loop
	net_shutdown(pipes);
	// stop logger event loop
	logger_shutdown(pipes);
	return 1;
}


struct ext_thread_arg
{
	struct pipes* pipes;
	FN_EXT_THREAD cb;
	void* ud;
};
static void ext_thread(struct ext_thread_arg* arg) 
{
	arg->pipes->extThreadNum.fetch_add(1);
	arg->cb(arg->pipes,arg->ud);
	arg->pipes->extThreadNum.fetch_sub(1);
	delete arg;
}
int pps_ext_thread(struct pipes* pipes, FN_EXT_THREAD cb, void* ud)
{
	using namespace std;
	struct ext_thread_arg* arg = new struct ext_thread_arg;
	arg->pipes = pipes;
	arg->ud = ud;
	arg->cb = cb;
	thread th = thread(ext_thread, arg);
	th.detach();
	return 1;	
}
void pps_wait_ext_thread_done(struct pipes* pipes)
{
	while (pipes->extThreadNum.load(std::memory_order_relaxed) > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}


