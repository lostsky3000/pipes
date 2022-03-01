#ifndef MQ_SPMC_H
#define MQ_SPMC_H

#include <cstdint>
#include <array>
#include <atomic>
#include <cassert>

#define SPMC_SLOTSTAT_EMPTY 0
#define SPMC_SLOTSTAT_CANREAD 1


template<typename T>
struct mq_spmc
{
	uint32_t capacity;
	uint32_t head;
	std::atomic<uint32_t> tail;
	std::atomic<int32_t> unReadNum;
	//
	T* slots;
	std::atomic<bool>* arrCanRead;
};

template<typename T>
	int spmc_push(const struct mq_spmc<T>* mq, T val, uint32_t* pLeft = nullptr)
	{
		struct mq_spmc<T>* q = const_cast<struct mq_spmc<T>*>(mq);
		//
		std::atomic<bool>& canRead = q->arrCanRead[q->head];
		if (canRead.load(std::memory_order_acquire)) {
			if (pLeft) {
				*pLeft = 0;
			}
			return 0;
		}
		q->slots[q->head] = val;
		if (++q->head >= q->capacity) {
			q->head = 0;
		}
		canRead.store(true, std::memory_order_release);
		if (pLeft) {
			int32_t num = q->unReadNum.fetch_add(1, std::memory_order_release) + 1;
			if (num < 0) {
				*pLeft = 0;
			}
			else {
				*pLeft = num;
			}
		}
		else {
			q->unReadNum.fetch_add(1, std::memory_order_release);
		}
		return 1;
	}

template<typename T>
int spmc_push_ptr(const struct mq_spmc<T>* mq, T* ptr, uint32_t* pLeft=nullptr)
{
	struct mq_spmc<T>* q = const_cast<struct mq_spmc<T>*>(mq);
	//
	std::atomic<bool>& canRead = q->arrCanRead[q->head];
	if (canRead.load(std::memory_order_acquire)) {
		if (pLeft) {
			*pLeft = 0;
		}
		return 0;
	}
	q->slots[q->head] = *ptr;
	if (++q->head >= q->capacity) {
		q->head = 0;
	}
	canRead.store(true, std::memory_order_release);
	if (pLeft) {
		int32_t num = q->unReadNum.fetch_add(1, std::memory_order_release) + 1;
		if (num < 0) {
			*pLeft = 0;
		}
		else {
			*pLeft = num;
		}
	}
	else {
		q->unReadNum.fetch_add(1, std::memory_order_release);
	}
	return 1;
}

template<typename T>
int spmc_pop(const struct mq_spmc<T>* mq, T* ptr, int32_t* pLeft=nullptr)
{
	struct mq_spmc<T>* q = const_cast<struct mq_spmc<T>*>(mq);
	//
	int32_t unReadNum = q->unReadNum.fetch_sub(1, std::memory_order_relaxed);
	if (unReadNum < 1) {
		q->unReadNum.fetch_add(1, std::memory_order_relaxed);
		if (pLeft) {
			*pLeft = 0;
		}
		return 0;
	}
	if (pLeft) {
		*pLeft = unReadNum - 1;
	}
	//
	uint32_t tailIdx = q->tail.fetch_add(1, std::memory_order_relaxed) & (q->capacity - 1);
	std::atomic<bool>& canRead = q->arrCanRead[tailIdx];
	assert(canRead.load(std::memory_order_acquire));
	*ptr = q->slots[tailIdx];
	
	canRead.store(false, std::memory_order_release);
	//assert(canRead.exchange(false, std::memory_order_release));
	
	return 1;
}

template<typename T>
void spmc_init_push_thread(const struct mq_spmc<T>* mq)
{
	mq->unReadNum.load(std::memory_order_acquire);
}

template<typename T>
uint32_t spmc_size(const struct mq_spmc<T>* mq)
{
	int32_t num = mq->unReadNum.load(std::memory_order_relaxed);
	if (num > 0) {
		return num;
	}
	return 0;
}

template<typename T>
const struct mq_spmc<T>* spmc_create(uint32_t capacity)
{
	// check capacity is pow2
	uint32_t sz = 2;
	while (1) {
		if (sz == capacity) {
			break;
		}
		if (sz > capacity) {
			assert(false);
		}
		sz = sz << 1;
	}
	//
	struct mq_spmc<T>* mq = new struct mq_spmc<T>();
	mq->capacity = capacity;
	mq->head = 0;
	mq->slots = new T[capacity];
	std::atomic<bool>* arrCanRead = new std::atomic<bool>[capacity];
	mq->arrCanRead = arrCanRead;
	for (uint32_t i=0; i<capacity; ++i) {
		arrCanRead[i].store(false, std::memory_order_release);
	}
	mq->tail.store(0, std::memory_order_release);
	mq->unReadNum.store(0, std::memory_order_release);
	return mq;		
}

template<typename T>
void spmc_destroy(const struct mq_spmc<T>* mq)
{
	mq->unReadNum.load(std::memory_order_acquire);
	delete[] mq->arrCanRead;
	delete[] mq->slots;
	delete mq;		
}

#endif // MQ_SPMC_H

