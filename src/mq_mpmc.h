#ifndef MQ_MPMC_H
#define MQ_MPMC_H

#include <cassert>
#include <cstdint>
#include <array>
#include <atomic>


//
#define MPMC_SLOTSTAT_EMPTY 0
#define MPMC_SLOTSTAT_CANREAD 1
#define MPMC_SLOTSTAT_READING 2

template<typename T>
struct mq_mpmc
{
	
	std::atomic<bool> readLock;
	
	uint32_t capacity;
	std::atomic<uint32_t> unReadNum;
	
	uint32_t tail;
	
	std::atomic<uint8_t>* arrSlotStat;
	T* slots;
	
	std::atomic<uint64_t> head;
};

static void readlock_init(std::atomic<bool>& lock) {
	lock.store(false);
}
static bool read_trylock(std::atomic<bool>& lock) {
	return !lock.load(std::memory_order_relaxed)
		&& !lock.exchange(true, std::memory_order_acquire);
}
static void read_unlock(std::atomic<bool>& lock) {
	lock.store(false, std::memory_order_release);
}

template<typename T>
int mpmc_push(const struct mq_mpmc<T>* mq, T val)
{	
	return mpmc_push_ptr(mq, &val);
}

template<typename T>
int mpmc_push_ptr(const struct mq_mpmc<T>* mq, T* ptr)
{
	struct mq_mpmc<T>* q = const_cast<struct mq_mpmc<T>*>(mq);
	//
	uint32_t unReadNum = q->unReadNum.fetch_add(1, std::memory_order_relaxed);
	uint32_t capacity = q->capacity;
	if (unReadNum >= capacity) {
		q->unReadNum.fetch_sub(1, std::memory_order_relaxed);
		return 0;
	}
	//
	uint32_t headIdx = q->head.fetch_add(1, std::memory_order_relaxed) & (capacity - 1);
	
	q->slots[headIdx] = *ptr;
	
	std::atomic<uint8_t>& stat = q->arrSlotStat[headIdx];
	
	//assert(stat.exchange(MPMC_SLOTSTAT_CANREAD, std::memory_order_release) == MPMC_SLOTSTAT_EMPTY);
	stat.store(MPMC_SLOTSTAT_CANREAD, std::memory_order_release);
	
	return 1;
}

template<typename T>
int mpmc_pop(const struct mq_mpmc<T>* mq, T* ptr)
{
	struct mq_mpmc<T>* q = const_cast<struct mq_mpmc<T>*>(mq);
	//
	std::atomic<bool>& readLock = q->readLock;
	if (!read_trylock(readLock)) {
		return 0;
	}
	uint32_t capacity = q->capacity;
	uint32_t tailIdx = q->tail & (capacity - 1);
	std::atomic<uint8_t>& stat = q->arrSlotStat[tailIdx];
	if (stat.load(std::memory_order_acquire) != MPMC_SLOTSTAT_CANREAD) { // no data
		read_unlock(readLock);
		return 0;
	}
	//
	*ptr = q->slots[tailIdx];
	stat.store(MPMC_SLOTSTAT_EMPTY, std::memory_order_release);
	
	if (++q->tail >= capacity) {
		++q->tail = 0;
	}
	read_unlock(readLock);
	
	q->unReadNum.fetch_sub(1, std::memory_order_release);
	return 1;
}

template<typename T>
uint32_t mpmc_size(const struct mq_mpmc<T>* mq)
{
	uint32_t num = mq->unReadNum.load(std::memory_order_acquire);
	if (num < mq->capacity) {
		return num;
	}
	return mq->capacity;
}

template<typename T>
const struct mq_mpmc<T>* mpmc_create(uint32_t capacity)
{	// check capacity is pow2
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
	struct mq_mpmc<T>* mq = new struct mq_mpmc<T>();
	readlock_init(mq->readLock);
	read_trylock(mq->readLock);
	//
	mq->capacity = capacity;
	mq->arrSlotStat = new std::atomic<uint8_t>[capacity];
	mq->slots = new T[capacity];
	/**/
	for (uint32_t i = 0; i < capacity; ++i) {
		mq->arrSlotStat[i].store(MPMC_SLOTSTAT_EMPTY, std::memory_order_release);
	}
	//
	mq->tail = 0;
	read_unlock(mq->readLock);
	//
	mq->head.store(0, std::memory_order_release);
	mq->unReadNum.store(0, std::memory_order_release);
	
	return mq;
}

template<typename T>
void mpmc_destroy(const struct mq_mpmc<T>* mq)
{
	mq->unReadNum.load(std::memory_order_acquire);
	delete[] mq->arrSlotStat;
	delete[] mq->slots;
	delete mq;
}


#endif // !MQ_MPMC_H








