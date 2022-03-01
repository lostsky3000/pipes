#ifndef PPS_SPIN_LOCK_H
#define PPS_SPIN_LOCK_H

#include <cassert>

#define SPIN_INIT(q) spinlock_init(q)
#define SPIN_LOCK(q) spinlock_lock(q)
#define SPIN_UNLOCK(q) spinlock_unlock(q)
#define SPIN_TRYLOCK(q) spinlock_trylock(q)


// spin lock

#ifdef USE_PTHREAD_SPIN


#else
#include <atomic>
#include <emmintrin.h>
struct spin_lock
{
	/*
	#if __cplusplus <= 201703L   // before c++20
	std::atomic_flag lock = ATOMIC_FLAG_INIT;
	#else
	std::atomic_flag lock;   // no need init after c++20
	#endif
	*/
	std::atomic<int> lock;
};

static inline void spinlock_init(struct spin_lock* lock)
{
	lock->lock.store(0, std::memory_order_relaxed);
}

static inline void spinlock_lock(struct spin_lock* lock)
{
	for (;;) {
		while (lock->lock.load(std::memory_order_relaxed)) {
			_mm_pause();
		}
		if (!lock->lock.exchange(1, std::memory_order_acquire)) {
			return;
		}
	}
}

static inline int spinlock_trylock(struct spin_lock* lock)
{
	return !lock->lock.load(std::memory_order_relaxed)
		&& !lock->lock.exchange(1, std::memory_order_acquire);
}

static inline void spinlock_unlock(struct spin_lock* lock)
{
	lock->lock.store(0, std::memory_order_release);
}

#endif // USE_PTHREAD_SPIN


// mcs lock
#define SPIN_MCS_INIT(q) spin_mcs_init(q)
#define SPIN_MCS_LOCK(q,nd) spin_mcs_lock(q, nd)
#define SPIN_MCS_UNLOCK(q,nd) spin_mcs_unlock(q, nd)
#define SPIN_MCS_TRYLOCK(q,nd) spin_mcs_trylock(q, nd)

#include <atomic>
struct spin_mcs_node
{
	std::atomic<struct spin_mcs_node*> next;
	std::atomic<uint8_t> locked;
	uint8_t in_queue = 0;
};
struct spin_mcs_lock
{
	std::atomic<struct spin_mcs_node*> tail;
};
static inline void spin_mcs_init(struct spin_mcs_lock* lock) 
{
	lock->tail.store(nullptr, std::memory_order_relaxed);
}
static inline int _mcs_enqueue(struct spin_mcs_lock* lock, struct spin_mcs_node* node)
{
	assert(!node->in_queue);
	node->in_queue = 1;   //  mark in queue
	// reset node
	if (node->next.load(std::memory_order_relaxed)) {
		node->next.store(nullptr, std::memory_order_relaxed);
	}
	// fetch prev node
	struct spin_mcs_node* prev = lock->tail.exchange(node, std::memory_order_acquire);
	if (prev == nullptr) {
		return 1;
	}
	if (node->locked.load(std::memory_order_relaxed)) {
		node->locked.store(0, std::memory_order_relaxed);
	}
	// link to prev node
	prev->next.store(node, std::memory_order_relaxed);
	return 0;
}
static inline void _mcs_wait_busy(struct spin_mcs_lock* lock, struct spin_mcs_node* node)
{
	while (!node->locked.load(std::memory_order_relaxed)) {
		_mm_pause();
	}
	lock->tail.load(std::memory_order_acquire);
}
static inline int _mcs_wait_once(struct spin_mcs_lock* lock, struct spin_mcs_node* node)
{
	if (node->locked.load(std::memory_order_relaxed)) {
		lock->tail.load(std::memory_order_acquire);
		return 1;
	}
	return 0;
}
static inline void spin_mcs_lock(struct spin_mcs_lock* lock, struct spin_mcs_node* node)
{
	// node enqueue
	if (_mcs_enqueue(lock, node)) {
		return;
	}
	// spin wait locked
	_mcs_wait_busy(lock, node);
}
static inline int spin_mcs_trylock(struct spin_mcs_lock* lock, struct spin_mcs_node* node)
{
	if (!node->in_queue) {  // not in queue, enqueue
		if ( _mcs_enqueue(lock, node) ) {  // get lock
			return 1;
		}
	}
	// wait once
	return _mcs_wait_once(lock, node);
}
static inline void spin_mcs_unlock(struct spin_mcs_lock* lock, struct spin_mcs_node* node)
{
	assert(node->in_queue);
	node->in_queue = 0;
	struct spin_mcs_node* next = node->next.load(std::memory_order_relaxed);
	if (!next) {
		struct spin_mcs_node* nodeCp = node;
		if (lock->tail.compare_exchange_strong(nodeCp, nullptr, std::memory_order_release)) {
			return;
		}
		while ( !(next=node->next.load(std::memory_order_relaxed)) ) {
			_mm_pause();
		}
	}
	lock->tail.fetch_add(0, std::memory_order_release);
	next->locked.store(1, std::memory_order_relaxed);
}

#endif // !PPS_SPIN_LOCK_H

