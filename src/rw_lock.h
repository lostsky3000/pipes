#ifndef RW_LOCK_H
#define RW_LOCK_H

#include <atomic>
#include <cstdint>
#include <cassert>

#define RWLOCK_WRITE_FLAG -100000
struct rw_lock
{
	std::atomic<int32_t> cnt;
};

inline struct rw_lock* rwlock_new()
{
	struct rw_lock* lock = new struct rw_lock;
	lock->cnt.store(0);
	return lock;
}
inline void rwlock_destroy(struct rw_lock* lock)
{
	delete lock;
}
//
inline int rwlock_write_trylock(struct rw_lock* lock)
{
	int32_t expect = 0;
	if(lock->cnt.compare_exchange_weak(expect, RWLOCK_WRITE_FLAG, std::memory_order_acquire)){
		return 1;
	}
	return 0;
}
inline void rwlock_write_lock(struct rw_lock* lock)
{
	while(!rwlock_write_trylock(lock)){}
}
inline void rwlock_write_unlock(struct rw_lock* lock)
{
	int32_t expect = RWLOCK_WRITE_FLAG;
	if(!lock->cnt.compare_exchange_weak(expect, 0, std::memory_order_release)){  // maybe incr by other readtrylock
		expect = RWLOCK_WRITE_FLAG;
		while(!lock->cnt.compare_exchange_weak(expect, 0, std::memory_order_release)){
			expect = RWLOCK_WRITE_FLAG;
		}
	}
}
//
inline int rwlock_read_trylock(struct rw_lock* lock)
{
	std::atomic<int32_t>& cnt = lock->cnt;
	/**/
	if(cnt.load(std::memory_order_relaxed) < 0){  // broad check
		return 0;
	}
	if(cnt.fetch_add(1, std::memory_order_acquire) < 0){ // narrow check
		cnt.fetch_sub(1, std::memory_order_relaxed);
		return 0;
	}
	return 1;
}
inline void rwlock_read_lock(struct rw_lock* lock)
{
	while(!rwlock_read_trylock(lock)){}
}
inline void rwlock_read_unlock(struct rw_lock* lock)
{
	lock->cnt.fetch_sub(1, std::memory_order_release);
}

#endif // !RW_LOCK_H
