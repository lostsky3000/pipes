#include "test_portal.h"

//
#include <thread>
#include <climits>
#include <list>
#include <array>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>

#include "pps_sysinfo.h"
#include "pps_sysapi.h"
#include "pool_linked.h"
#include "ring_queue.h"
#include "spinlock.h"
#include "mq_mpsc.h"
#include "mq_mpmc.h"
#include "mq_spmc.h"
#include "test_mq_wait.h"
#include "pps_service.h"
#include "pps_message.h"
#include "mq_spsc.h"


static int g_flag1 = 1;
static int g_num2 = 0;
static void th_1()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	g_num2 = 42;
	g_flag1 = 0;
}
static void th_2()
{
	while (g_flag1) {
		//std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (g_num2 != 42) {
		int n = 1;
	}
}
static void threadTest1()
{
	std::thread th1 = std::thread(th_1);
	std::thread th2 = std::thread(th_2);
	th1.join();
	th2.join();
	int n = 1;
}

struct st_test
{
	int num;
	int has;
};

static void plkTest2()
{
	unsigned int seed = (unsigned int)time(NULL);
	srand(seed);
	const int randNum = 100000;
	int arrRand[randNum];
	for (int i = 0; i < randNum; ++i) {
		arrRand[i] = rand() % 10000;
	}
	//
	auto plk = plk_create<int>();
	std::list<int> ls;
	int cnt = 0;
	int times = 100000;
	// push test
	struct time_clock tm1;
	sysapi_clock_now(tm1);
	while (cnt++ < times) {
		plk_push(plk, cnt);
	}
	struct time_clock tm2;
	sysapi_clock_now(tm2);
	uint64_t cost1 = tm2 - tm1;
	cnt = 0;
	sysapi_clock_now(tm1);
	while (cnt++ < times) {
		ls.push_back(cnt);
	}
	sysapi_clock_now(tm2);
	uint64_t cost2 = tm2 - tm1;
	// it test
	int rndIdx = 0;
	sysapi_clock_now(tm1);
	const plk_iterator<int> itPlk = plk_it_init(plk);
	for (; plk_it_loop(itPlk);) {
		int* p = *itPlk;
		if (arrRand[rndIdx ++% randNum] % 4 == 0) {
			  //del
			plk_erase(itPlk);
		}
	}
	sysapi_clock_now(tm2);
	cost1 = tm2 - tm1;
	rndIdx = 0;
	sysapi_clock_now(tm1);
	for (std::list<int>::iterator it = ls.begin(); it != ls.end();) {
		int num = *it;
		if (arrRand[rndIdx ++% randNum] % 4 == 0) {
			  //del
			it = ls.erase(it);
		}
		else {
			++it;
		}
	}
	sysapi_clock_now(tm2);
	cost2 = tm2 - tm1;
	//
	plk_destroy(plk);
	int n = 1;
}
static int plkTestCompare(const struct pool_linked<struct st_test>* plk,
	std::list<struct st_test*>& ls)
{
	int sz1 = plk_size(plk);
	int sz2 = ls.size();
	if (sz1 != sz2) {
		int n = 1;
	}
	std::list<struct st_test*>::iterator itLs = ls.begin();
	const struct plk_iterator<struct st_test> itPlk = plk_it_init(plk);
	int cnt = 0;
	while (itLs != ls.end()) {
		int ret = plk_it_loop(itPlk);
		if (!ret) {
			int n = 1;
		}
		struct st_test* p = *itLs;
		struct st_test* v = *itPlk;
		if (p->num != v->num) {
			int n = 1;
		}
		++itLs;
		++cnt;
	}
	return 1;
}
static void plkTest()
{
	unsigned int seed = (unsigned int)time(NULL);
	//seed = 1629271358;
	srand(seed);
	int cap1 = rand() % 1000;
	int cap2 = rand() % 1000;
	const struct pool_linked<struct st_test>* plk1 = plk_create<struct st_test>(cap1);
	const struct pool_linked<struct st_test>* plk2 = plk_create<struct st_test>(cap2);
	std::list<struct st_test*> ls1;
	std::list<struct st_test*> ls2;
	int cnt = 0;
	struct st_test stWrap;
	while (cnt++ < 20000) {
		if (cnt == 82) {
			   // debug
			int n = 1;
		}
		int act = rand() % 7;
		if (act > 2) {
			 // push
			struct st_test* p = new struct st_test;
			p->num = cnt;
			p->has = 1;
			plk_push_ptr(plk1, p);
			ls1.push_back(p);
		}
		else if (act == 0) {
			  // pop
			int ret = plk_pop(plk1, &stWrap);
			if (ret) {
				  // has pop sth
				struct st_test* p = ls1.front();
				ls1.pop_front();
				if (p->num != stWrap.num) {
					int n = 1;
				}
				delete p;
			}
			int sz = plk_size(plk1);
			int sz2 = ls1.size();
			if (sz != sz2) {
				int n = 1;
			}
		}
		else if (act == 1) {
			  // rand erase
			int sz = plk_size(plk1);
			if (sz > 0) {
				int bingo = rand() % sz;
				int idx = 0;
				struct plk_iterator<struct st_test> it = plk_it_init(plk1);
				std::list<struct st_test*>::iterator itLs = ls1.begin();
				bool del = false;
				while (plk_it_loop(it)) {
					if (idx++ == bingo) {
						del = true;
					}
					if (del) {
						struct st_test* p1 = *itLs;
						struct st_test* st1 = *it;
						if (p1->num != st1->num) {
							int n = 1;
						}
						plk_erase(it);
						itLs = ls1.erase(itLs);
						delete p1;
						del = false;
					}
					else {
						++itLs;
					}
					if (idx > bingo) {
						if (rand() % 2 == 0) {
							break;
						}
						else {  
							del = rand() % 4 == 0;    //del next
						}
					}
					//
					sz = plk_size(plk1);
					int sz2 = ls1.size();
					if (sz != sz2) {
						int n = 1;
					}
				}
				plkTestCompare(plk1, ls1);
			}
		}
		else if (act == 2) {
			   //move wrap
			int sz = plk_size(plk1);
			if (sz > 0) {
				int sz2 = ls1.size();
				if (sz != sz2) {
					int n = 1;
				}
				int bingo = rand() % sz;
				int idx = 0;
				const struct plk_iterator<struct st_test> itPlk = plk_it_init(plk1);
				std::list<struct st_test*>::iterator itLs = ls1.begin();
				bool del = false;
				while (plk_it_loop(itPlk)) {
					if (idx++ == bingo) {
						del = true;
					}
					if (del) {
						const struct plk_item_wrap<struct st_test>* wrap = plk_erase_wrap(itPlk);
						struct st_test* pLs = *itLs;
						itLs = ls1.erase(itLs);
						// add to plk2 & ls2
						plk_push_wrap(plk2, wrap);
						ls2.push_back(pLs);
						//
						del = false;
					}
					else {
						++itLs;
					}
					if (idx > bingo) {
						if (rand() % 2 == 0) {
							break;
						}
						else {
							del = rand() % 4 == 0;
						}
					}
				}
			}
		}
		//
		int sz1 = plk_size(plk1);
		int sz2 = plk_size(plk2);
		if (sz1 != ls1.size()) {
			int n = 1;
		}
		if (sz2 != ls2.size()) {
			int n = 1;
		}
	}
	plkTestCompare(plk1, ls1);
	plkTestCompare(plk2, ls2);
	// destroy
	plk_destroy(plk1);
	plk_destroy(plk2);
	for (struct st_test* p : ls1) {
		delete p;
	}
	for (struct st_test* p : ls2) {
		delete p;
	}
}

static int fn_rq_dbg1(void* pvItem, int has, void* pItem)
{
	struct st_test* pv = (struct st_test*)pvItem;
	struct st_test* pi = (struct st_test*)pItem;
	if (has) {
		if (pv->num != pi->num) {
			return 0;
		}
	}
	else {
		if (pv && pv->has) {
			return 0;
		}
	}
	return 1;
}
static void rqTest2()
{
	unsigned int seed = (unsigned int)time(NULL);
	srand(seed);
	std::vector<struct st_test*> vec;
	const struct ring_queue<st_test>* rq = rq_create<st_test>(1);
	int cnt = 0;
	int vItemNum = 0;
	struct st_test stWrap;
	while (cnt++ < 30000) {
		//struct st_test2 st;
		int act = rand() % 4;
		if (act == 0 || act == 1) {
			  //push
			struct st_test* stTmp = new struct st_test;
			stTmp->num = cnt;
			if (rand() % 3 == 0) {
				  // val is null
				stTmp->has = 0;
				rq_push(rq);
			}
			else {
				stTmp->has = 1;
				rq_push(rq, stTmp);
				++vItemNum;
			}
			vec.push_back(stTmp);
			// debug
			//rq_dbg_compare(rq, vec, fn_rq_dbg1);
		}
		else if (act == 2) {
			 // pop
			if(vec.size() == 0) {
				
			}
			else {
				struct st_test* pSt = vec.at(0);
				vec.erase(vec.begin());
				if (pSt && pSt->has) {
					--vItemNum;
				}
				int ret = rq_pop(rq, &stWrap);
				delete pSt;
				// debug
				//rq_dbg_compare(rq, vec, fn_rq_dbg1);
			}
		}
		else if (act == 3) {
			  // set & resize
			int sz = rq_size(rq);
			if (rand() % 2 == 0) {
				  //set
				int idx = rand() % (1 + sz + rand() % (sz / 4 + 1));
				if (idx >= sz) {
					   // need expand
					int szNew = idx + 1;
					rq_expand(rq, szNew);
					vec.resize(szNew);
				}
				struct st_test* pvSt = vec.at(idx);
				if (rand() % 3 == 0) {
					   // set null
					rq_set(rq, idx);
					if (pvSt) {
						if (pvSt->has) {
							--vItemNum;
						}
						pvSt->has = 0;
					}
					// debug
					//rq_dbg_compare(rq, vec, fn_rq_dbg1);
				}
				else {
					   // set val
					if(pvSt) {
						if (!pvSt->has) {
							++vItemNum;
						}
						pvSt->num = cnt;
						pvSt->has = 1;
						rq_set(rq, idx, pvSt);
					}
					else {
						struct st_test* pSt = new struct st_test;
						pSt->num = cnt;
						pSt->has = 1;
						rq_set(rq, idx, pSt);
						vec[idx] = pSt;
						++vItemNum;
					}
				}
			}
			else {
				   // resize
				int szNew = sz + 1 + rand() % 6;
				rq_expand(rq, szNew);
				vec.resize(szNew);
				// debug
				//rq_dbg_compare(rq, vec, fn_rq_dbg1);
			}
		}
		//
		int sz = vec.size();
		if (sz > 0) {
			struct st_test* pSt = vec.at(0);
			int vIsNull = (pSt == nullptr || !pSt->has);
			int ret = rq_front(rq, &stWrap);
			if ((vIsNull && ret) || (!vIsNull && !ret)) {
				int n = 1;
			}
			if (!vIsNull && pSt->num != stWrap.num) {
				int n = 1;
			}
			// test rq_get
			int idx = rand() % sz;
			pSt = vec.at(idx);
			vIsNull = (pSt == nullptr || !pSt->has);
			ret = rq_get(rq, idx, &stWrap);
			if ((vIsNull && ret) || (!vIsNull && !ret)) {
				int n = 1;
			}
			if (!vIsNull && pSt->num != stWrap.num) {
				int n = 1;
			}
		}
		if (rq_item_num(rq) != vItemNum) {
			int n = 1;
		}
	}
	//
	rq_dbg_compare(rq, vec, fn_rq_dbg1);
	//
	rq_destroy(rq);
	//
	for(struct st_test* p : vec) {
		if (p) {
			delete p;
		}
	}
}

static void rqTest() 
{
	std::list<int> ls2;

	std::atomic<uint32_t> unum1(UINT_MAX);
	uint32_t num1 = unum1.fetch_add(1, std::memory_order_relaxed);
	num1 = unum1.fetch_add(1, std::memory_order_relaxed);
	num1 = unum1.fetch_add(1, std::memory_order_relaxed);
	unsigned int seed = (unsigned int)time(NULL);
	srand(seed);
	std::list<int> lsTest;
	const struct ring_queue<st_test>* rq = rq_create<st_test>(1);
	int cnt = 0;
	struct st_test st2;
	int base = 1 + rand() % 3;
	while (cnt++ < 30000) {
		if (cnt % 1000 == 0) {
			base = 1 + rand() % 3;
		}
		st2.num = cnt;
		if (rand() % base == 0) {
			  // push
			rq_push(rq, &st2);
			lsTest.push_back(st2.num);
		}
		else {
			  // pop
			if(rq_pop(rq, &st2)) {
				lsTest.pop_front();
			}
		}
		if (rq_size(rq) != lsTest.size()) {
			int n = 1;
		}
	}
	int n = rq_size(rq);
	n = 1;
	while (rq_pop(rq, &st2)) {
		int num = lsTest.front();
		lsTest.pop_front();
		if (num != st2.num) {
			n = 1;   //exception
		}
	}
	//
	rq_destroy(rq);

}

// mcs spin
struct st_mcs_node
{
	std::atomic<struct st_mcs_node*> next;
	std::atomic<int> locked;
}
;
static std::atomic<struct st_mcs_node*> g_mcsLock;
static int g_mcsNum = 0;
static void mcs_lock(struct st_mcs_node*node) 
{
	// reset node
	node->locked.store(0, std::memory_order_relaxed);
	node->next.store(nullptr, std::memory_order_relaxed);
	//
	struct st_mcs_node* prev = g_mcsLock.exchange(node, std::memory_order_relaxed);
	if (prev == nullptr) {
		  // queue is empty, get lock
		return;
	}
	// set prev->next
	prev->next.store(node, std::memory_order_relaxed);
	// spin
	while(!node->locked.load(std::memory_order_acquire)) {

	}
}
static void mcs_unlock(struct st_mcs_node*node)
{
	struct st_mcs_node* next = node->next.load(std::memory_order_relaxed);
	if (!next) {
		struct st_mcs_node*nodeCp = node;
		if (g_mcsLock.compare_exchange_strong(nodeCp, nullptr, std::memory_order_release)) {
			return;
		}
		while (!(next = node->next.load(std::memory_order_relaxed))) {

		}
	}
	next->locked.store(1, std::memory_order_release);
}

static struct spin_mcs_lock g_spinMcsLock;
static struct spin_lock g_spinLock;
static std::mutex g_spinMtx;

//#define DBG_MCS_LOCK
//#define DBG_MCS_LOCK_TRY
//#define DBG_MCS_MTX
#define DBG_MCS_SPIN

static void mcs_thread(int* pIdx) 
{
	int idx = *pIdx;
	struct st_mcs_node mcsNode;
	struct spin_mcs_node node;
	//
	for(int i = 0 ; i < 100 ; ++i) {
		for (int j = 0; j < 10000; ++j) {
#ifdef DBG_MCS_LOCK
			SPIN_MCS_LOCK(&g_spinMcsLock, &node);
#elif defined(DBG_MCS_LOCK_TRY)
			while (!SPIN_MCS_TRYLOCK(&g_spinMcsLock, &node)) {}
#elif defined(DBG_MCS_MTX)
			g_spinMtx.lock();
#elif defined(DBG_MCS_SPIN)
			SPIN_LOCK(&g_spinLock);
#endif	
			if (idx % 2 == 0) {
				++g_mcsNum;
			}
			else {
				--g_mcsNum;
			}
#ifdef DBG_MCS_LOCK
			SPIN_MCS_UNLOCK(&g_spinMcsLock, &node);
#elif defined(DBG_MCS_LOCK_TRY)
			SPIN_MCS_UNLOCK(&g_spinMcsLock, &node);
#elif defined(DBG_MCS_MTX)
			g_spinMtx.unlock();
#elif defined(DBG_MCS_SPIN)
			SPIN_UNLOCK(&g_spinLock);
#endif	
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
	}
}

static void mcsTest() 
{
	struct st_test stNum1;
	struct st_test* pStNum1 = &stNum1;
	std::atomic<struct st_test*> atmNum1;
	atmNum1.store(pStNum1);
	ptrdiff_t ptrDiff = 0;
	atmNum1.fetch_add(ptrDiff);

	// init lock
	g_mcsLock.store(nullptr, std::memory_order_relaxed);

	SPIN_MCS_INIT(&g_spinMcsLock);

	SPIN_INIT(&g_spinLock);

	// start thread
	std::thread arrThread[32];
	int arrThIdx[32];
	int thNum = 4;
	//
	struct time_clock tmBegin;
	struct time_clock tmEnd;
	sysapi_clock_now(tmBegin);
	for (uint32_t i = 0; i < thNum; ++i) {
		arrThIdx[i] = i;
		arrThread[i] = std::thread(mcs_thread, &arrThIdx[i]);
	}
	for (uint32_t i = 0; i < thNum; ++i) {
		arrThread[i].join();
	}
	sysapi_clock_now(tmEnd);
	uint64_t tmCost = tmEnd - tmBegin;
	//tmCost = sysapi_clock_since_epoch(tmEnd);
	//
	if(g_mcsNum != 0) {
		int n = 1;
	}
}

 
// mq test
//#define TEST_MQ_MPSC
//#define TEST_MQ_WAIT_SINGLE_CONSUMER
//
//#define TEST_MQ_SPMC
//#define TEST_MQ_WAIT_SINGLE_PRODUCER
//
//#define TEST_MQ_MPMC

#define TEST_MQ_SPSC

//#define TEST_MQ_DEBUG

//
//#define TEST_MQDEBUG_RESULT
#include <set>
static std::list<int32_t> g_mqLsDbgRet;
static std::mutex g_mqMtxDbgRet;

static const uint32_t g_mqLoopTimes = 10;
static const uint32_t g_mqCap = 1024; //4096 * 4096;
static const uint32_t g_mqMsgNumPerLoop = 4096 * 4096 - 10; //g_mqCap - 1; //10000000;   //
static std::atomic <uint32_t> g_mqLoopCnt(0);
static std::atomic<bool> g_mqThQuitFlag(false);
//
static struct mq_mpsc<struct st_test>* g_mqMpsc = nullptr;
static struct test_mq_wait<struct st_test, g_mqCap>* g_mqWait = nullptr;
static struct mq_mpmc<struct st_test>* g_mqMpmc = nullptr;
static struct mq_spmc<struct st_test>* g_mqSpmc = nullptr;
static struct mq_spsc<struct st_test>* g_mqSpsc = nullptr;
//
static uint32_t g_mqThNum = 0;
static std::atomic<uint32_t> g_mqThReadyNum(0);
static std::atomic<uint32_t> g_mqConsumerNum(0);
static std::atomic<uint32_t> g_mqProducerNum(0);
static std::atomic<bool> g_mqStartFlag(false);
//
static std::atomic<uint32_t> g_mqConsumerMsgCnt(0);
static std::atomic<uint32_t> g_mqProducerMsgCnt(0);
static std::atomic<uint64_t> g_mqCostTotalConsumer(0);
static std::atomic<uint64_t> g_mqCostTotalProducer(0);
static std::atomic<uint64_t> g_mqTmReadBegin(0);
static std::atomic<uint64_t> g_mqTmSendBegin(0);

static std::mutex g_mqMtxConsume;
static std::unordered_set<uint32_t> g_mqSetConsume;

static int _mq_consume_add(uint32_t val)
{
	int ret;
	g_mqMtxConsume.lock();
	if (g_mqSetConsume.find(val) == g_mqSetConsume.end())  // not found
		{
			ret = 1;
		}
	else
	{
		ret = 0;
	}
	g_mqSetConsume.insert(val);
	g_mqMtxConsume.unlock();
	return ret;
}
static void _mq_consume_clear()
{
	g_mqMtxConsume.lock();
	g_mqSetConsume.clear();
	g_mqMtxConsume.unlock();
}

static void mq_test_thread(int* pIdx)
{
	int thIdx = *pIdx;
#if defined(TEST_MQ_SPMC) or defined(TEST_MQ_WAIT_SINGLE_PRODUCER)    // single producer
	bool isConsumer = (thIdx != 0);
#elif defined(TEST_MQ_MPSC) or defined(TEST_MQ_WAIT_SINGLE_CONSUMER)   // single consumer
	bool isConsumer = (thIdx == 0);
#else
	bool isConsumer = (thIdx % 2 == 0);  // multi-p & multi-c
	//bool isConsumer = (thIdx == 0);   // single-c
	//bool isConsumer = (thIdx != 0);    // single-p
#endif
	
#if defined(TEST_MQ_MPSC)
	mpsc_init_pop_thread(g_mqMpsc);
#elif defined(TEST_MQ_SPMC)
	spmc_init_push_thread(g_mqSpmc);
#endif

	
	uint32_t typeIdx = 0;
	if (isConsumer)
	{
		typeIdx = g_mqConsumerNum.fetch_add(1);
	}
	else
	{
		typeIdx = g_mqProducerNum.fetch_add(1);
	}
	//
	g_mqThReadyNum.fetch_add(1);
	if (thIdx == 0)
	{
		while (g_mqThReadyNum.load() < g_mqThNum) {}
		// all thread ready
		printf("all thread ready, consumerNum=%d, producerNum=%d\n",
			g_mqConsumerNum.load(),
			g_mqProducerNum.load());
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		g_mqStartFlag.store(true);
	}
	else
	{
		while (!g_mqStartFlag.load()) {}
	}
	struct st_test stWrap;
	struct time_clock tmTmp;
	if (isConsumer)  // consumer
		{
			printf("consumer(%d) start, thread=%d\n", typeIdx, thIdx);
			while (1)
			{
#if defined(TEST_MQ_MPSC)
				int ret = mpsc_pop(g_mqMpsc, &stWrap);
#elif defined(TEST_MQ_SPMC)
				int ret = spmc_pop(g_mqSpmc, &stWrap);
#elif defined(TEST_MQ_MPMC)
				int ret = mpmc_pop(g_mqMpmc, &stWrap);
#elif defined(TEST_MQ_SPSC)
				int ret = spsc_pop(g_mqSpsc, &stWrap);
#else
				int ret = test_mqwait_pop(g_mqWait, &stWrap);
#endif
				if (ret)  // has msg
					{
#if defined(TEST_MQ_DEBUG)
						if (!_mq_consume_add(stWrap.num))   // debug
							{
								printf("consume bug, ret=%d\n", stWrap.num);
								assert(false);
							}
#endif
						
#if defined(TEST_MQDEBUG_RESULT)
						{
							std::unique_lock<std::mutex> lock(g_mqMtxDbgRet);
							g_mqLsDbgRet.push_back(stWrap.num);
						}
#endif
						//
						uint32_t cnt = g_mqConsumerMsgCnt.fetch_add(1);
						if (cnt % 100000 == 0) {
							//printf("readCnt=%d\n", cnt);
						}
						if (cnt == 0)   // 1st msg
							{
								sysapi_clock_now(tmTmp);
								g_mqTmReadBegin.store(sysapi_clock_since_epoch(tmTmp));
							}
						else if (cnt == g_mqMsgNumPerLoop - 1)  // last msg
							{
								sysapi_clock_now(tmTmp);
								uint64_t tmCost = sysapi_clock_since_epoch(tmTmp) - g_mqTmReadBegin.load();
								g_mqCostTotalConsumer.fetch_add(tmCost);
								printf("consumerCost(%d)=%ld\n", g_mqLoopCnt.load(), tmCost);
								// start next loop
								while(g_mqProducerMsgCnt.load() < g_mqMsgNumPerLoop)
								{
									 // wait all producer done
								}
								//
#if defined(TEST_MQDEBUG_RESULT)
								{
									int32_t duplCnt = 0;
									std::set<int32_t> setDup;
									std::set<int32_t> setOri;
									for (int32_t i = 0; i < g_mqMsgNumPerLoop; ++i) {
										setOri.insert(i);
									}
									std::unique_lock<std::mutex> lock(g_mqMtxDbgRet);
									size_t szLs = g_mqLsDbgRet.size();
									if (szLs != g_mqMsgNumPerLoop) {
										printf("dbgLsSizeInvalid: %lu\n", szLs);
									}
									for (std::list<int32_t>::iterator it = g_mqLsDbgRet.begin();
										it != g_mqLsDbgRet.end(); ++it) {
											int32_t tmpN = *it;
											if (setDup.find(tmpN) != setDup.end()) {
												++duplCnt;
												//assert(false);
												printf("dup:%d, ",tmpN);
											}
											setDup.insert(tmpN);
											//
											std::set<int32_t>::iterator itOri = setOri.find(tmpN);
											if (itOri != setOri.end()) {
												setOri.erase(itOri);
											}
									}
									g_mqLsDbgRet.clear();
									if (duplCnt > 0) {
										printf("================================ debugRet, duplNum=%d\n", duplCnt);
									}
									if (setOri.size() > 0) {
										printf("noOri: ");
										for (std::set<int32_t>::iterator it=setOri.begin();
										it != setOri.end(); ++it) {
											printf("%d, ",*it);
										}
										printf("\n");
									}
								}
#endif
								//
								if (g_mqLoopCnt.fetch_add(1) < g_mqLoopTimes - 1) // continue next loop
									{
										//printf("start next loop: %d, ", g_mqLoopCnt.load());
										std::this_thread::sleep_for(std::chrono::milliseconds(1000));
#if defined(TEST_MQ_DEBUG)
										_mq_consume_clear();    // debug
#endif
				g_mqConsumerMsgCnt.store(0);
										g_mqProducerMsgCnt.store(0);
									}
								else  // finish all job
									{
										printf("consumer found all job done, will quit\n");
										g_mqThQuitFlag.store(true);
									}
							}
					}
				else
				{
					if (g_mqThQuitFlag.load())
					{
						break;
					}	
				}
			}
			printf("consumer(%d) done, thread=%d\n", typeIdx, thIdx);
		}
	else  // producer
		{	
			printf("producer(%d) start, thread=%d\n", typeIdx, thIdx);
			while (1)
			{
				uint32_t cnt = g_mqProducerMsgCnt.load();
				if (cnt < g_mqMsgNumPerLoop) //can producer msg
					{
						bool doSend = true;
						cnt = g_mqProducerMsgCnt.fetch_add(1);
						if (cnt % 100000 == 0) {
							//printf("sendCnt=%d\n", cnt);
						}
						if (cnt == 0)   // 1st msg
							{
								sysapi_clock_now(tmTmp);
								g_mqTmSendBegin.store(sysapi_clock_since_epoch(tmTmp));
							}
						else if (cnt == g_mqMsgNumPerLoop - 1)  //last msg
							{
								sysapi_clock_now(tmTmp);
								uint64_t tmCost = sysapi_clock_since_epoch(tmTmp) - g_mqTmSendBegin.load();
								g_mqCostTotalProducer.fetch_add(tmCost);
								printf("producerCost(%d)=%ld, ", g_mqLoopCnt.load(), tmCost);
							}
						else if (cnt >= g_mqMsgNumPerLoop)  // do nothing
							{
								doSend = false;
							}
						if (doSend)
						{
								// send msg
							stWrap.num = cnt;
#if defined(TEST_MQ_MPSC)
							while (!mpsc_push_ptr(g_mqMpsc, &stWrap)) {}
							;
#elif defined(TEST_MQ_SPMC)
							while (!spmc_push_ptr(g_mqSpmc, &stWrap)) {}
							;
#elif defined(TEST_MQ_MPMC)
							while (!mpmc_push_ptr(g_mqMpmc, &stWrap)) {}
							;
#elif defined(TEST_MQ_SPSC)
							while (!spsc_push_ptr(g_mqSpsc, &stWrap)) {}
							;
#else
							while (!test_mqwait_push_ptr(g_mqWait, &stWrap)) {}
							;
#endif	
						}
					}
				if (g_mqThQuitFlag.load())
				{
					break;
				}	
			}
			printf("producer(%d) done, thread=%d\n", typeIdx, thIdx);
		}
}

static void mq_test()
{	
#if defined(TEST_MQ_MPSC)
	const struct mq_mpsc<struct st_test>*cq = mpsc_create<struct st_test>(g_mqCap);
	g_mqMpsc = const_cast<struct mq_mpsc<struct st_test>*>(cq); 
#elif defined(TEST_MQ_SPMC)
	const struct mq_spmc<struct st_test>* cq = 
		spmc_create<struct st_test>(g_mqCap);
	g_mqSpmc = const_cast<struct mq_spmc<struct st_test>*>(cq);
#elif defined(TEST_MQ_MPMC)
	const struct mq_mpmc<struct st_test>* cq =
			mpmc_create<struct st_test>(g_mqCap);
	g_mqMpmc = const_cast<struct mq_mpmc<struct st_test>*>(cq);
#elif defined(TEST_MQ_SPSC)
	g_mqSpsc = spsc_create<struct st_test>(1024);
#else
	const struct test_mq_wait<struct st_test, g_mqCap>* cq =
			test_mq_create<struct st_test, g_mqCap>();
	g_mqWait = const_cast<struct test_mq_wait<struct st_test, g_mqCap>*>(cq);
#endif
	//
	uint32_t coreNum = sysinfo_core_num();
	g_mqThNum = 2; //coreNum;
	printf("coreNum=%d, threadNum=%d, msgNumPerLoop=%d, loopTimes=%d, ", 
		coreNum,
		g_mqThNum,
		g_mqMsgNumPerLoop,
		g_mqLoopTimes);
#if defined(TEST_MQ_MPSC)
	printf("noWait mpsc\n");
#elif defined(TEST_MQ_SPMC)
	printf("noWait spmc\n");
#elif defined(TEST_MQ_MPMC)
	printf("noWait mpmc\n");
#elif defined(TEST_MQ_SPSC)
	printf("noWait spsc\n");
#elif defined(TEST_MQ_WAIT_MUTEX)
	printf("wait mtx\n");
#else
	printf("wait spin\n");
#endif  
	
#if defined(TEST_MQDEBUG_RESULT)
	{
		std::unique_lock<std::mutex> lock(g_mqMtxDbgRet);
		g_mqLsDbgRet.clear();
	}
#endif
	//
	std::atomic<uint32_t> testAtm(0);
	printf("atomic is-lock-free: %d\n", testAtm.is_lock_free());
	//
	std::thread arrTh[256];
	int arrArg[256];
	struct time_clock tmBegin, tmEnd;
	sysapi_clock_now(tmBegin);
	for (int i = 0; i < g_mqThNum; ++i) {
		arrArg[i] = i;
		arrTh[i] = std::thread(mq_test_thread, &arrArg[i]);
	}
	for (int i = 0; i < g_mqThNum; ++i) {
		arrTh[i].join();
	}
	sysapi_clock_now(tmEnd);
	uint64_t tmCostTotal = tmEnd - tmBegin;
	//
	printf("all done, ");
	//
#if defined(TEST_MQ_MPSC)
	printf("noWait mpsc, ");
	mpsc_destroy(g_mqMpsc);
#elif defined(TEST_MQ_SPMC)
	printf("noWait spmc, ");
	spmc_destroy(g_mqSpmc);
#elif defined(TEST_MQ_MPMC)
	printf("noWait mpmc, ");
	mpmc_destroy(g_mqMpmc);
#elif defined(TEST_MQ_SPSC)
	printf("noWait spsc, ");
	spsc_destroy(g_mqSpsc);
#else
#if defined(TEST_MQ_WAIT_MUTEX)
	printf("wait mtx, ");
#else
	printf("wait spin, ");
#endif
	test_mqwait_destroy(g_mqWait);
#endif

	//
	uint64_t avrCostConsume = g_mqCostTotalConsumer.load() / g_mqLoopTimes;
	uint64_t avrCostProduce = g_mqCostTotalProducer.load() / g_mqLoopTimes;
	printf("avrProduce=%ld, avrConsume=%ld\n", avrCostProduce, avrCostConsume);
	//
	int n = 1;
}

// minheap test begin
#include "test_thpool.h"
#include "minheap.h"
struct mhp_st 
{
	int num;
};
static int minHeapCompare(struct mhp_st* st1, struct mhp_st* st2)
{
	if (st1->num < st2->num) {
		return -1;
	}
	if (st1->num > st2->num) {
		return 1;
	}
	return 0;
}
static void minHeapTest()
{
	int loopTimes = 100;
	int totalNum = 10000000;
	unsigned int seed = (unsigned int)time(NULL);
	srand(seed);
	std::vector<struct mhp_st*> vec;
	printf("mhpTest start, totalNum=%d, loopTimes=%d\n", totalNum, loopTimes);
	// init
	for(int i = 0 ; i < totalNum ; ++i) { 
		struct mhp_st* st = new struct mhp_st;
		st->num = i;
		vec.push_back(st);
	}
	struct minheap_queue<struct mhp_st*>* mhp = minheap_create<struct mhp_st*>(10, minHeapCompare);
	//
	for (int m=0;m<loopTimes;++m) {
		// shuffle
		for(int i = 0 ; i < totalNum - 1 ; ++i) {
			int idxDst = i + 1 + rand() % (totalNum - i - 1);
			int tmp = vec[i]->num;
			vec[i]->num = vec[idxDst]->num;
			vec[idxDst]->num = tmp;
		}
		// add to mhp
		for(int i = 0 ; i < totalNum ; ++i) {
			minheap_add(vec[i], mhp);
		}
		// check
		int checkCnt = 0;
		struct mhp_st* st;
		while ((st = minheap_pop(mhp))) {
			if (st->num != checkCnt) {
				assert(false);
			}
			++checkCnt;
		}
		//
		std::list<struct mhp_st*> lsUnAdd;
		for(int i = 0 ; i < totalNum ; ++i) {
			int rnd = rand() % 4;
			if (rnd == 0 || rnd == 1) {
				minheap_add(vec[i], mhp);
			}
			else if (rnd == 2) {
				lsUnAdd.push_back(vec[i]);
			}
			else {
				st = minheap_pop(mhp);
				if (st) {
					lsUnAdd.push_back(st);
				}
				--i;
			}
		}
		for (std::list<struct mhp_st*>::iterator it = lsUnAdd.begin(); it != lsUnAdd.end(); ++it) {
			minheap_add(*it, mhp);
		}
		// check
		checkCnt = 0;
		while ((st = minheap_pop(mhp))) {
			if (st->num != checkCnt) {
				assert(false);
			}
			++checkCnt;
		}
		//
		printf("mhpTest loop(%d) done\n", m);
	}
	size_t sz = vec.size();
	for (int i=0; i<sz;++i) {
		delete vec[i];
	}
	minheap_destroy(mhp);
	printf("mhpTest all done\n");
}
// minheap test end

// timing wheel test begin
#include "timing_wheel.h"
struct tmw_st
{
	int64_t expiration;
};
int64_t g_tmwTmNow = 0;
uint64_t g_tmwTotalOff = 0;
uint64_t g_tmwCbCnt = 0;
void static tmw_callback(struct tmw_st* s, int64_t tmNow)
{
	int off = g_tmwTmNow - s->expiration;
	if (off <= -15 || off >= 15) {
		assert(false);
	}
	else {
		//printf("off = %d\n", off);
	}
	if (off < 0) {
		g_tmwTotalOff -= off;
	}
	else {
		g_tmwTotalOff += off;
	}
	++g_tmwCbCnt;
}
void tmwheelTest()
{
	unsigned int seed = (unsigned int)time(NULL);
	srand(seed);
	printf("tmwTest start\n");
	int64_t tmBegin = 10000000;
	int64_t dur = 1000000;
	int64_t tmEnd = tmBegin + dur;
	int64_t tmStep = 2;
	g_tmwTmNow = tmBegin;
	int initTask = 200000;
	//
	struct tw_timer<struct tmw_st*>* timer = tmwheel_create<struct tmw_st*>(1, 1000, 
		tmBegin, tmw_callback, 1000, 1000);
	// add init task
	for(int i=0; i<initTask; ++i) {
		int64_t expire = rand() % (dur + dur*1/3);
		struct tmw_st* st = new struct tmw_st;
		st->expiration = g_tmwTmNow + expire;
		tmwheel_add(st->expiration, g_tmwTmNow, st, timer);
	}
	// advance clock
	while(g_tmwTmNow < tmEnd) {
		int rnd = rand() % 2;
		if (rnd == 0) {   // add task
			int num = 1 + rand() % 10;
			for (int i=0; i<num; ++i) {
				int64_t tmLeft = tmEnd - g_tmwTmNow;
				int64_t dlt = rand() % (tmLeft);
				struct tmw_st* st = new struct tmw_st;
				st->expiration = g_tmwTmNow + dlt;
				tmwheel_add(st->expiration, g_tmwTmNow, st, timer);
			}
		}
		tmwheel_advance_clock(g_tmwTmNow, timer);
		g_tmwTmNow += tmStep;
	}
	tmwheel_destroy(timer);
	float offAvr = g_tmwTotalOff * 1.0f / g_tmwCbCnt;
	printf("tmwTest all done, cbNum=%ld, offAvr=%f\n", g_tmwCbCnt, offAvr);
}
// timing wheel test end


static void test_thread()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(3000));
	FILE* f = fopen("/root/test.txt", "a");
	fprintf(f, "aaa1\n");
	fclose(f);
}

// spsc test begin

static struct mq_spsc<int>* s_mqSpsc;
static void spsc_thread(int idx)
{
	int total = 100000000;
	int cnt;
	if(idx == 0){   // consumer
		spsc_read_thread_acquire(s_mqSpsc);
 		cnt = 0;
		int val;
		while(true){
			if(spsc_pop(s_mqSpsc, &val)){
				if(val != cnt){
					int n = 1;
				}
				++cnt;
			}else{
				if(cnt >= total){
					std::this_thread::sleep_for(std::chrono::milliseconds(3000));
					if(spsc_pop(s_mqSpsc, &val)){
						int n = 1;
					}
					break;
				}
			}
		}
		printf("spsc_thread consumer done\n");
	}else{  // producer
		spsc_write_thread_acquire(s_mqSpsc);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		cnt = 0;
		while(cnt < total){
			spsc_push(s_mqSpsc, cnt++);	
			if(cnt%123210 == 0){
				//std::this_thread::sleep_for(std::chrono::milliseconds(0));
			}
		}
		printf("spsc_thread producer done\n");
	}
}
static void spsc_test()
{
	s_mqSpsc = spsc_create<int>(16);
	std::thread th1 = std::thread(spsc_thread, 0);
	std::thread th2 = std::thread(spsc_thread, 1);
	th1.join();
	th2.join();
	
	int n = 1;
}

// spsc test end

#include "tcp_decode.h"
#include "dec_http_head.h"
#include "util_crypt.h"
void test_portal_start()
{
	return;
	int n = 1;
	struct dec_sep* dec = decsep_new();
	const char* sep = "\n\r";
	decsep_reset(dec, sep, strlen(sep));
	const char* testStr1 = "123456789";
	const char* testStr2 = "12345\n\r67";
	int readBytes;
	n = decsep_check(dec, testStr1, strlen(testStr1), readBytes);
	n = decsep_check(dec, testStr2, strlen(testStr2), readBytes);
	n = 1;
	// ws sec test
	const char* strWsSec = "5psC1FKyvTLZ1i+t3eaw+g==";
	const char* WSSECKEY = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	int szWsSec = strlen(strWsSec);
	int szSecKey = strlen(WSSECKEY);
	int szBufEnc = szWsSec + szSecKey;
	char* encBuf = new char[szBufEnc];
	memcpy(encBuf, strWsSec, szWsSec);
	memcpy(encBuf + szWsSec, WSSECKEY, szSecKey);

	uint8_t digest[SHA1_DIGEST_SIZE];
	ucrypt_sha1((uint8_t*)encBuf, szBufEnc, digest);
	n = ucrypt_b64encode_calcsz(SHA1_DIGEST_SIZE);
	char bufTmp[1024];
	ucrypt_b64encode(digest, SHA1_DIGEST_SIZE, bufTmp);
	bufTmp[n] = '\0';
	delete[] encBuf;
	//
	const char* strReqLine = "GET /12345 HTTP/1.1\r\n";
	const char* strHead1 = "host:test\r\n";
	const char* strHead2 = "name:hehe\r\n";
	const char* strHead3 = "age:123\r\n\r\n";
	struct dec_http_head* decHttp = dechh_new();
	n = dechh_tick(decHttp, strReqLine, strlen(strReqLine), readBytes);
	n = dechh_tick(decHttp, strHead1, strlen(strHead1), readBytes);
	n = dechh_tick(decHttp, strHead2, strlen(strHead2), readBytes);
	n = dechh_tick(decHttp, strHead3, strlen(strHead3), readBytes);

	const char* method = dechh_method(decHttp, &n);
	const char* url = dechh_url(decHttp, &n);
	const char* ver = dechh_ver(decHttp, &n);

	char* pVal;
	int keyLen, valLen;
	dechh_headerit_init(decHttp);
	const char* key1 = dechh_headerit_next(decHttp, &keyLen, &pVal, &valLen);
	const char* key2 = dechh_headerit_next(decHttp, &keyLen, &pVal, &valLen);
	const char* key3 = dechh_headerit_next(decHttp, &keyLen, &pVal, &valLen);
	const char* key4 = dechh_headerit_next(decHttp, &keyLen, &pVal, &valLen);
	n = 1;

	//spsc_test();
	//std::thread th = std::thread(test_thread);	
	//th.join();
	//th.detach();
	
	//rqTest();
	//rqTest2();

	//plkTest();
	//plkTest2();

	//threadTest1();

	//mcsTest();

	mq_test();
	
	//test_thpool();
	
	//minHeapTest();

	//tmwheelTest();
	std::unordered_set<int> setTest;
	setTest.insert(1);
	setTest.insert(1);
	setTest.erase(1);
	setTest.erase(1);
	
	printf("dadada\n");
	
	uint32_t msgType = 7;
	uint32_t szReal = 2034519;
	uint32_t szCombine = msg_size_combine(msgType, szReal);
	uint32_t msgType2 = msg_type(szCombine);
	uint32_t szReal2 = msg_size_real(szCombine);
	
	n = 1;
}


