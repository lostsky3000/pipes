#ifndef TEST_MQ_WAIT_H
#define TEST_MQ_WAIT_H

#include <cstdint>
#include <cassert>
#include <array>


#define TEST_MQ_WAIT_MUTEX


#if defined(TEST_MQ_WAIT_MUTEX)
#include <mutex>
template<typename T, uint32_t capacity_>
struct test_mq_wait
{
	std::array<T, capacity_> queue;
	uint32_t head;
	uint32_t tail;
	uint32_t unReadNum;
	std::mutex mtx;
};
#else   //use spin
#include "spinlock.h"
template<typename T, uint32_t capacity_>
struct test_mq_wait
{
	std::array<T, capacity_> queue;
	uint32_t head;
	uint32_t tail;
	uint32_t unReadNum;
	struct spin_lock lock;
};

#endif


template<typename T, uint32_t capacity_>
int test_mqwait_push_ptr(const struct test_mq_wait<T, capacity_>* mq, T* ptr)
{
	struct test_mq_wait<T, capacity_>* q = const_cast<struct test_mq_wait<T, capacity_>*>(mq);
#if defined(TEST_MQ_WAIT_MUTEX)
	q->mtx.lock();
#else
	SPIN_LOCK(&q->lock);
#endif
	//
	if (q->unReadNum >= capacity_) {  //  queue is full
#if defined(TEST_MQ_WAIT_MUTEX)
		q->mtx.unlock();
#else
		SPIN_UNLOCK(&q->lock);
#endif
		return 0;
	}
	//
	uint32_t head = q->head & (capacity_ - 1);
	q->queue[head] = *ptr;
	++q->head;
	++q->unReadNum;
	//
#if defined(TEST_MQ_WAIT_MUTEX)
	q->mtx.unlock();
#else
	SPIN_UNLOCK(&q->lock);
#endif
	return 1;
}

template<typename T, uint32_t capacity_>
int test_mqwait_pop(const struct test_mq_wait<T, capacity_>* mq, T* wrap)
{
	struct test_mq_wait<T, capacity_>* q = const_cast<struct test_mq_wait<T, capacity_>*>(mq);
#if defined(TEST_MQ_WAIT_MUTEX)
	q->mtx.lock();
#else
	SPIN_LOCK(&q->lock);
#endif
	//
	if (q->unReadNum <= 0) {   // queue is empty
#if defined(TEST_MQ_WAIT_MUTEX)
		q->mtx.unlock();
#else
		SPIN_UNLOCK(&q->lock);
#endif
		return 0;
	}
	//
	*wrap = q->queue[q->tail];
	--q->unReadNum;
	if (++q->tail >= capacity_) {
		q->tail = 0;
	}
	//
#if defined(TEST_MQ_WAIT_MUTEX)
	q->mtx.unlock();
#else
	SPIN_UNLOCK(&q->lock);
#endif
	return 1;
}


template<typename T, uint32_t capacity_>
const struct test_mq_wait<T, capacity_>* test_mq_create()
{
	// check capacity is pow2
	uint32_t sz = 2;
	while (1) {
		if (sz == capacity_) {
			break;
		}
		if (sz > capacity_) {
			assert(false);
		}
		sz = sz << 1;
	}
	struct test_mq_wait<T, capacity_>* q = new struct test_mq_wait<T, capacity_>();
#if defined(TEST_MQ_WAIT_MUTEX)
	q->mtx.lock();
#else
	SPIN_INIT(&q->lock);
	SPIN_LOCK(&q->lock);
#endif
	//
	q->head = 0;
	q->tail = 0;
	q->unReadNum = 0;
	//
#if defined(TEST_MQ_WAIT_MUTEX)
	q->mtx.unlock();
#else
	SPIN_UNLOCK(&q->lock);
#endif
	return q;
}

template<typename T, uint32_t capacity_>
void test_mqwait_destroy(const struct test_mq_wait<T, capacity_>* q)
{
	delete q;
}



#endif // !TEST_MQ_WAIT_H




