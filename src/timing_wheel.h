#ifndef TIMING_WHEEL_H
#define TIMING_WHEEL_H

#include <cstdint>
#include <cstdlib>
#include <cassert>
#include "minheap.h"

#define TW_NODE_IDX_ROOT -1
#define TW_NODE_IDX_NULL -2

template<typename T>
struct tw_task
{
	int64_t expiration;
	T udata;
	int32_t prev;
	int32_t next;
	int32_t pool_idx;
};

template<typename T>
struct tw_slot
{
	int64_t expiration;
	struct tw_task<T>* root;
};

template<typename T>
struct tw_wheel
{
	uint32_t wheel_size;
	uint64_t tick_dur;
	uint64_t interval;
	int64_t cur_time;
	struct tw_slot<T>** slots;
	struct tw_wheel<T>* up_wheel;
};

template<typename T>
struct tw_timer
{
	//fn_tmwheel_callback fn_callback;
	void(*fn_callback)(T, int64_t);
	struct tw_wheel<T>* root;
	struct minheap_queue<struct tw_slot<T>*>* delay_queue;
	//
	struct tw_task<T>* task_pool;
	int32_t pool_cap;
	int32_t free_num;
	int32_t* free_pool;
	//
	uint32_t wheel_depth;
	//
	uint32_t total_task;
};

template<typename T>
static void add_slot_to_queue(struct tw_slot<T>* slot, struct tw_timer<T>* timer)
{
	minheap_add(slot, timer->delay_queue);
}

template<typename T>
static struct tw_task<T>* alloc_task_wrap(struct tw_timer<T>* timer)
{	
	if (timer->free_num <= 0)   // pool is full, no free wrap, resize
		{
			int oldCap = timer->pool_cap;
			struct tw_task<T>* oldPool = timer->task_pool;
			int32_t* oldFreePool = timer->free_pool;
			//
			timer->pool_cap *= 2;
			//size_t sz = sizeof(struct tw_task) * timer->pool_cap;
			timer->task_pool = new struct tw_task<T>[timer->pool_cap];  // timer->fn_malloc(sz);
			//sz = sizeof(int) * timer->pool_cap;
			timer->free_pool = new int[timer->pool_cap];   //timer->fn_malloc(sz);
			//memcpy(timer->task_pool, oldPool, sizeof(struct tw_task)*oldCap);
			for(int i = 0 ; i < oldCap; ++i) {
				timer->task_pool[i] = oldPool[i];
			}
			for (int i = oldCap; i < timer->pool_cap; ++i)
			{
				timer->task_pool[i].pool_idx = i;
				timer->free_pool[i - oldCap] = i;
			}
			timer->free_num = oldCap;
			//
			delete[] oldPool;  // timer->fn_free(oldPool);
			delete[] oldFreePool; //timer->fn_free(oldFreePool);
			//
		}
	struct tw_task<T>* task = &timer->task_pool[timer->free_pool[--timer->free_num]];
	task->next = TW_NODE_IDX_NULL;
	task->prev = TW_NODE_IDX_NULL;
	return task;
}

template<typename T>
static void free_task_wrap(struct tw_task<T>* task, struct tw_timer<T>* timer)
{
	assert(timer->free_num < timer->pool_cap);
	timer->free_pool[timer->free_num++] = task->pool_idx;
}

template<typename T>
static int add_task_to_wheel(struct tw_task<T>* task, struct tw_wheel<T>* wheel, 
	struct tw_timer<T>* timer)
{
	if (task->expiration < wheel->cur_time + wheel->tick_dur)
	{
		//already expired
		return 0;
	}
	else if (task->expiration < wheel->cur_time + wheel->interval)
	{
		// add to cur wheel
		uint64_t idSlot = (task->expiration / wheel->tick_dur);
		struct tw_slot<T>* slot = wheel->slots[idSlot % wheel->wheel_size];
		add_task_to_slot(task, slot, timer);
		if (set_slot_expiration(idSlot * wheel->tick_dur, slot, timer))
		{
			add_slot_to_queue(slot, timer);
		}
		return 1;
	}
	else
	{
		// add to up wheel
		if(wheel->up_wheel == nullptr) 
		{
			//create up wheel
			wheel->up_wheel = create_wheel(wheel->interval, wheel->wheel_size, wheel->cur_time, timer);
		}
		return add_task_to_wheel(task, wheel->up_wheel, timer);
	}
}

template<typename T>
static void add_task(struct tw_task<T>* task, struct tw_timer<T>* timer, int64_t tmNow)
{
	int ret = add_task_to_wheel(task, timer->root, timer);
	if (!ret)   // expired, call back & free task-wrap
		{
			//expired, callback
			timer->fn_callback(task->udata, tmNow);
			//free task-wrap
			free_task_wrap(task, timer);
		}
}
template<typename T>
static inline struct tw_task<T>* trans_idx_to_node(int idx, struct tw_task<T>* root, struct tw_timer<T>* timer)
{
	if (idx == TW_NODE_IDX_ROOT)
	{
		return root;
	}
	if (idx >= 0)
	{
		return &timer->task_pool[idx];
	}
	return nullptr;
}
template<typename T>
static inline int trans_node_to_idx(struct tw_task<T>* node, struct tw_task<T>* root)
{
	if (node == root)
	{
		return TW_NODE_IDX_ROOT;
	}
	if (node == nullptr)
	{
		return TW_NODE_IDX_NULL;
	}
	return node->pool_idx;
}
template<typename T>
static inline int set_slot_expiration(int64_t expiration, struct tw_slot<T>* slot, struct tw_timer<T>* timer)
{
	if (expiration != slot->expiration)
	{
		slot->expiration = expiration;
		return 1;
	}
	return 0;
}
template<typename T>
static inline void add_task_to_slot(struct tw_task<T>* task, struct tw_slot<T>* slot, 
	struct tw_timer<T>* timer)
{
	struct tw_task<T>* root = slot->root;
	struct tw_task<T>* tail = trans_idx_to_node(root->prev, root, timer); 
	task->next = trans_node_to_idx(root, root);
	task->prev = trans_node_to_idx(tail, root);
	tail->next = trans_node_to_idx(task, root);
	root->prev = tail->next;
	++timer->total_task;
	/*
	 *		val tail = root.prev
            timerTaskEntry.next = root
            timerTaskEntry.prev = tail
            tail.next = timerTaskEntry
            root.prev = timerTaskEntry
	 **/
}

template<typename T>
static inline void remove_task_from_slot(struct tw_task<T>* task, struct tw_slot<T>* slot, 
	struct tw_timer<T>* timer)
{
	struct tw_task<T>* root = slot->root;
	trans_idx_to_node(task->next, root, timer)->prev = task->prev;
	trans_idx_to_node(task->prev, root, timer)->next = task->next;
	task->next = TW_NODE_IDX_NULL;
	task->prev = TW_NODE_IDX_NULL;
	--timer->total_task;
	/*
	      timerTaskEntry.next.prev = timerTaskEntry.prev
          timerTaskEntry.prev.next = timerTaskEntry.next
          timerTaskEntry.next = null
          timerTaskEntry.prev = null
	 */
}
template<typename T>
static inline void flush_slot(struct tw_slot<T>* slot, struct tw_timer<T>* timer, int64_t tmNow)
{
	struct tw_task<T>* root = slot->root;
	struct tw_task<T>* head = trans_idx_to_node(root->next, root, timer);
	// reset expiration
	set_slot_expiration(-1, slot, timer);
	//
	while(head != root)
	{
		remove_task_from_slot(head, slot, timer);
		// re-add
		add_task(head, timer, tmNow);
		//
		head = trans_idx_to_node(root->next, root, timer);
	}
	/*
	 *var head = root.next
      while (head ne root) {
        remove(head)
        f(head)
        head = root.next
      }
      expiration.set(-1L)
	 **/
}
template<typename T>
static inline struct tw_slot<T>* create_slot(struct tw_timer<T>* timer)
{
	struct tw_slot<T>* slot = new struct tw_slot<T>;
	// init root node
	struct tw_task<T>* root = new struct tw_task<T>;
	slot->root = root;
	root->pool_idx = TW_NODE_IDX_ROOT;
	root->next = TW_NODE_IDX_ROOT;
	root->prev = TW_NODE_IDX_ROOT;
	root->expiration = -1;
	
	set_slot_expiration(-1, slot, timer);
	
	return slot;
}

template<typename T>
static inline struct tw_wheel<T>* create_wheel(uint64_t tickDur, uint32_t wheelSize, 
	int64_t startTime, struct tw_timer<T>* timer)
{	
	struct tw_wheel<T>* wheel = new struct tw_wheel<T>;
	wheel->tick_dur = tickDur;
	wheel->wheel_size = wheelSize;
	wheel->interval = tickDur * wheelSize;
	wheel->cur_time = startTime - startTime % tickDur;
	//create slots
	wheel->slots = new struct tw_slot<T>*[wheel->wheel_size];
	for (int i = 0; i < wheel->wheel_size; ++i)
	{
		wheel->slots[i] = create_slot(timer);
	}
	wheel->up_wheel = nullptr;
	++timer->wheel_depth;
	return wheel;
}

template<typename T>
static inline void advance_clock(int64_t tmNow, struct tw_wheel<T>* wheel, 
	struct tw_timer<T>* timer)
{
	if (tmNow >= wheel->cur_time + wheel->tick_dur) // can tick, update time
		{
			wheel->cur_time = tmNow - (tmNow % wheel->tick_dur);
			//advance up-wheel
			if(wheel->up_wheel)
			{
				advance_clock(wheel->cur_time, wheel->up_wheel, timer);
			}
		}
}

template<typename T>
static int slot_compare(struct tw_slot<T>* s1, struct tw_slot<T>* s2)
{
	if (s1->expiration < s2->expiration)
	{
		return -1;
	}
	if (s1->expiration > s2->expiration)
	{
		return 1;
	}
	return 0;
}

//
template<typename T>
struct tw_timer<T>* tmwheel_create(
	uint32_t tickDur,
	uint32_t wheelSize,
	int64_t startTime,
	void(*fnCallback)(T, int64_t),
	uint32_t taskPoolCap,
	uint32_t delayQueueCap)
{
	assert(tickDur > 0);
	assert(wheelSize > 0);
	assert(taskPoolCap > 0);
	assert(delayQueueCap > 0);
	//
	struct tw_timer<T>* timer = new struct tw_timer<T>;
	//memset(timer, 0, sz);
	//
	timer->fn_callback = fnCallback;
	//
	timer->root = create_wheel(tickDur, wheelSize, startTime, timer);
	timer->wheel_depth = 1;
	//
	timer->delay_queue = minheap_create<struct tw_slot<T>*>(delayQueueCap, slot_compare);
	// init task pool
	timer->pool_cap = taskPoolCap;
	//sz = sizeof(struct tw_task) * timer->pool_cap;
	timer->task_pool = new struct tw_task<T>[timer->pool_cap];
	// init free pool
	//sz = sizeof(int) * timer->pool_cap;
	timer->free_pool = new int[timer->pool_cap];
	for (int i = 0; i < timer->pool_cap; ++i)
	{
		timer->task_pool[i].pool_idx = i;
		timer->free_pool[i] = i;
	}
	timer->free_num = timer->pool_cap;
	
	return timer;
}

template<typename T>
void tmwheel_add(int64_t expiration, int64_t tmNow, T udata, struct tw_timer<T>* timer)
{
	struct tw_task<T>* task = alloc_task_wrap(timer);
	task->expiration = expiration;
	task->udata = udata;
	//
	add_task(task, timer, tmNow);
}

template<typename T>
int64_t tmwheel_advance_clock(int64_t tmNow, struct tw_timer<T>* timer)
{
	struct tw_slot<T>* slot;
	while ((slot = minheap_get(timer->delay_queue)))
	{
		int64_t off = slot->expiration - tmNow;
		if (off <= 0)   // expired
			{
				minheap_pop(timer->delay_queue);     // pop from delay-queue
				advance_clock(slot->expiration, timer->root, timer);     // advance time-wheel
				flush_slot(slot, timer, tmNow);      // flush cur slot
			}
		else   // return slot delayed time
			{
				return off;
			}
	}
	// no slot in delay-queue, return 0
	return 0;
}

template<typename T>
void tmwheel_destroy(struct tw_timer<T>* timer)
{
	struct tw_wheel<T>* curWheel = timer->root;
	while (curWheel) {
		if (curWheel->slots) {
			for (uint32_t i=0; i<curWheel->wheel_size;++i) {
				struct tw_slot<T>* slot = curWheel->slots[i];
				delete slot->root;
				delete slot;
			}
			delete[] curWheel->slots;
			curWheel->slots = nullptr;
		}
		//
		struct tw_wheel<T>* upWheel = curWheel->up_wheel;
		delete curWheel;
		curWheel = upWheel;
	}
	timer->root = nullptr;
	if (timer->task_pool) {
		delete[] timer->task_pool;
		timer->task_pool = nullptr;
	}
	if (timer->free_pool) {
		delete[] timer->free_pool;
		timer->free_pool = nullptr;
	}
	if (timer->delay_queue) {
		minheap_destroy(timer->delay_queue);
		timer->delay_queue = nullptr;
	}
	delete timer;
}

template<typename T>
uint32_t tmwheel_size(struct tw_timer<T>* timer)
{
	return timer->total_task;
}


#endif // !TIMING_WHEEL_H

