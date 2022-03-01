#ifndef MQ_MPSC_H
#define MQ_MPSC_H

/*
  multi-producers,single-consumer mq
*/

#include <cstdint>
#include <atomic>
#include <array>
#include <assert.h>

template<typename T>
struct mq_mpsc
{
	uint32_t capacity;
	uint32_t tail;
	std::atomic<uint32_t> unReadNum;
	T* slots;
	std::atomic<bool>* arrCanRead;
	std::atomic<uint64_t> head;
};

template<typename T>
int mpsc_push(const struct mq_mpsc<T>* mq, T val, uint32_t* pLeft = nullptr)
{
	struct mq_mpsc<T>* q = const_cast<struct mq_mpsc<T>*>(mq);
	uint32_t num = q->unReadNum.fetch_add(1, std::memory_order_relaxed);
	uint32_t capacity = q->capacity;
	if (num >= capacity) {
		// full
		q->unReadNum.fetch_sub(1, std::memory_order_relaxed);
		if (pLeft) {
			*pLeft = capacity;
		}
		return 0;
	}
	if (pLeft)
	{
		*pLeft = num + 1;
	}
	uint32_t headIdx = q->head.fetch_add(1, std::memory_order_relaxed) & (capacity - 1);
	std::atomic<bool>& canRead = q->arrCanRead[headIdx];
	q->slots[headIdx] = val;
	//canRead.store(true, std::memory_order_release);
	assert(canRead.exchange(true, std::memory_order_release) == false);
			
	return 1;
}

template<typename T>
int mpsc_push_ptr(const struct mq_mpsc<T>* mq, T* ptr, uint32_t* pLeft = nullptr)
{
	struct mq_mpsc<T>* q = const_cast<struct mq_mpsc<T>*>(mq);
	uint32_t num = q->unReadNum.fetch_add(1, std::memory_order_relaxed);
	uint32_t capacity = q->capacity;
	if (num >= capacity) {// full
		q->unReadNum.fetch_sub(1, std::memory_order_relaxed);
		if (pLeft) {
			*pLeft = capacity;
		}
		return 0;
	}
	if (pLeft)
	{
		*pLeft = num + 1;
	}
	uint32_t headIdx = q->head.fetch_add(1, std::memory_order_relaxed) & (capacity - 1);
	std::atomic<bool>& canRead = q->arrCanRead[headIdx];
	q->slots[headIdx] = *ptr;
	
	//canRead.store(true, std::memory_order_release);
	assert(canRead.exchange(true, std::memory_order_release) == false);
	
	return 1;
}

template<typename T, typename UD>
int mpsc_push_ptr_custom(const struct mq_mpsc<T>* mq, UD* pUd, void(*fnFill)(T*,UD*))
{
	struct mq_mpsc<T>* q = const_cast<struct mq_mpsc<T>*>(mq);
	uint32_t num = q->unReadNum.fetch_add(1, std::memory_order_relaxed);
	uint32_t capacity = q->capacity;
	if (num >= capacity) { // full
		q->unReadNum.fetch_sub(1, std::memory_order_relaxed);
		return 0;
	}
	uint32_t headIdx = q->head.fetch_add(1, std::memory_order_relaxed) & (capacity - 1);
	std::atomic<bool>& canRead = q->arrCanRead[headIdx];
	fnFill(&q->slots[headIdx], pUd);
	
	//canRead.store(true, std::memory_order_release);
	assert(canRead.exchange(true, std::memory_order_release) == false);
	
	return 1;
}

template<typename T>
int mpsc_pop(const struct mq_mpsc<T>* mq, T* ptr, uint32_t* pLeft = nullptr)
{
	struct mq_mpsc<T>* q = const_cast<struct mq_mpsc<T>*>(mq);
	std::atomic<bool>& canRead = q->arrCanRead[q->tail];
	if (!canRead.load(std::memory_order_acquire)) {
		// no readable data
	    if(pLeft) {
			*pLeft = 0;
		}
		return 0;
	}
	uint32_t capacity = q->capacity;
	*ptr = q->slots[q->tail];
	if (++q->tail >= capacity) {
		q->tail = 0;
	}
	canRead.store(false, std::memory_order_release);
	if (pLeft)
	{
		int32_t num = q->unReadNum.fetch_sub(1, std::memory_order_release) - 1;
		if (num > capacity) {
			*pLeft = capacity;
		}
		else {
			*pLeft = num;
		}
	}
	else
	{
		q->unReadNum.fetch_sub(1, std::memory_order_release);	
	}
	return 1;
}

template<typename T>
int mpsc_pop_custom(const struct mq_mpsc<T>* mq, T* ptr, void(*fnFill)(T*, T*), uint32_t* pLeft = nullptr)
{
	struct mq_mpsc<T>* q = const_cast<struct mq_mpsc<T>*>(mq);
	std::atomic<bool>& canRead = q->arrCanRead[q->tail];
	if (!canRead.load(std::memory_order_acquire)) {
		// no readable data
		if(pLeft) {
			*pLeft = 0;
		}
		return 0;
	}
	uint32_t capacity = q->capacity;
	//*ptr = q->slots[q->tail];   // assign
	fnFill(&q->slots[q->tail], ptr);
	if (++q->tail >= capacity) {
		q->tail = 0;
	}
	canRead.store(false, std::memory_order_release);
	if (pLeft)
	{
		int32_t num = q->unReadNum.fetch_sub(1, std::memory_order_release) - 1;
		if (num > capacity) {
			*pLeft = capacity;
		}
		else {
			*pLeft = num;
		}
	}
	else
	{
		q->unReadNum.fetch_sub(1, std::memory_order_release);	
	}
	return 1;
}


template<typename T>
void mpsc_init_pop_thread(const struct mq_mpsc<T>* mq)
{
	mq->unReadNum.load(std::memory_order_acquire);
}

template<typename T>
uint32_t mpsc_size(const struct mq_mpsc<T>* mq)
{
	uint32_t num = mq->unReadNum.load(std::memory_order_acquire);
	if (num < mq->capacity) {
		return num;
	}
	return mq->capacity;
}

template<typename T>
uint32_t mpsc_capacity(const struct mq_mpsc<T>* mq)
{
	mq->unReadNum.load(std::memory_order_acquire);
	return mq->capacity;
}

template<typename T>
bool mpsc_check_capacity(uint32_t capacity) {
	uint32_t sz = 2;
	while (1) {
		if (sz == capacity) {
			return true;
		}
		if (sz > capacity) {
			return false;
		}
		sz = sz << 1;
	}
}

template<typename T>
const struct mq_mpsc<T>* mpsc_create(uint32_t capacity)
{
	// check capacity is pow2
	assert(mpsc_check_capacity<T>(capacity));
	//
	struct mq_mpsc<T>* mq = new struct mq_mpsc<T>();
	mq->capacity = capacity;
	mq->slots = new T[capacity];
	mq->arrCanRead = new std::atomic<bool>[capacity];
	mq->tail = 0;
	/**/
	for (uint32_t i=0; i<capacity;++i) {
		mq->arrCanRead[i].store(false, std::memory_order_release);
	}
	mq->head.store(0, std::memory_order_release);
	mq->unReadNum.store(0, std::memory_order_release);
	return mq;
}

template<typename T>
void mpsc_destroy(const struct mq_mpsc<T>* mq)
{
	mq->unReadNum.load(std::memory_order_acquire);
	delete[] mq->arrCanRead;
	delete[] mq->slots;
	delete mq;
}

#endif // !MQ_MPSC_H




