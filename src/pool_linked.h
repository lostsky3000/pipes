#ifndef POOL_LINKED_H
#define POOL_LINKED_H

#include <cstdint>
#include <assert.h>

//#define PLK_DEBUG

template<typename T>
struct plk_item_wrap
{
	T item;
	struct plk_item_wrap<T>* prev = nullptr;
	struct plk_item_wrap<T>* next = nullptr;
};

template<typename T>
struct plk_linked_queue
{
	struct plk_item_wrap<T>* head = nullptr;
	struct plk_item_wrap<T>* tail = nullptr;
	int32_t size = 0;
};

template<typename T>
struct plk_iterator
{
	struct plk_linked_queue<T>* queue_used;
	struct plk_linked_queue<T>* queue_free;
	struct plk_item_wrap<T>* cursor = nullptr;
	struct plk_item_wrap<T>* curval = nullptr;
	uint8_t can_erase = 0;
	T* operator*() const
	{
		if (curval) {
			return &curval->item;
		}
		assert(0);
		return nullptr;
	}
};

template<typename T>
struct pool_linked
{
	struct plk_linked_queue<T> queue_free;
	struct plk_linked_queue<T> queue_used;
	struct plk_iterator<T> it;
};

template<typename T>
static int dbg_queue_check(struct plk_linked_queue<T>* q)
{
	if (q->size < 0) {
		int n = 1;
	}else if (q->size == 0) {
		if (q->head != nullptr || q->tail != nullptr) {
			int n = 1;
		}
	}else if (q->size == 1) {
		if (q->head != q->tail || q->head == nullptr) {
			int n = 1;
		}
		else {
			if (q->head->prev != nullptr || q->head->next != nullptr) {
				int n = 1;
			}
		}
	}
	else {

	}
	return 1;
}

template<typename T>
const struct plk_iterator<T>& plk_it_init(const struct pool_linked<T>* plk)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	p->it.cursor = p->queue_used.head;
	p->it.curval = nullptr;
	p->it.can_erase = 0;
	return p->it;
}

template<typename T>
int plk_it_loop(const struct plk_iterator<T>& it)
{	
	struct plk_item_wrap<T>* cursor = it.cursor;
	struct plk_iterator<T>* pIt = const_cast<struct plk_iterator<T>*>(&it);
	if (cursor) {
		pIt->curval = cursor;
		pIt->cursor = cursor->next;
		pIt->can_erase = 1;

#ifdef PLK_DEBUG
		dbg_queue_check(pIt->queue_used);
#endif // PLK_DEBUG

		return 1;
	}
	pIt->can_erase = 0;
	return 0;
}

template<typename T>
int plk_erase(const struct plk_iterator<T>& it)
{
	assert(it.can_erase);
	struct plk_item_wrap<T>* cur = it.curval;
	assert(cur);
	struct plk_linked_queue<T>* q = it.queue_used;
	struct plk_item_wrap<T>* prev = cur->prev;
	struct plk_item_wrap<T>* next = cur->next;
	if (prev) {
		prev->next = next;
	}
	if (next) {
		next->prev = prev;
	}
	if (cur == q->head) {
		q->head = cur->next;
	}
	if (cur == q->tail) {
		q->tail = cur->prev;
	}
	--q->size;
	// back to free-queue
	queue_push(it.queue_free, cur);
	// invalidate curval
	struct plk_iterator<T>* pIt = const_cast<struct plk_iterator<T>*>(&it);
	pIt->curval = nullptr;
	pIt->can_erase = 0;

#ifdef PLK_DEBUG
	dbg_queue_check(pIt->queue_used);
#endif // PLK_DEBUG

	return 1;
}

template<typename T>
const struct plk_item_wrap<T>* plk_erase_wrap(const struct plk_iterator<T>& it)
{
	struct plk_item_wrap<T>* cur = it.curval;
	assert(cur);
	struct plk_linked_queue<T>* q = it.queue_used;
	struct plk_item_wrap<T>* prev = cur->prev;
	struct plk_item_wrap<T>* next = cur->next;
	if (prev) {
		prev->next = next;
	}
	if (next) {
		next->prev = prev;
	}
	if (cur == q->head) {
		q->head = cur->next;
	}
	if (cur == q->tail) {
		q->tail = cur->prev;
	}
	--q->size;
	// invalidate curval
	struct plk_iterator<T>* pIt = const_cast<struct plk_iterator<T>*>(&it);
	pIt->curval = nullptr;

#ifdef PLK_DEBUG
	dbg_queue_check(pIt->queue_used);
#endif // PLK_DEBUG

	return cur;
}

template<typename T>
static inline void queue_push(struct plk_linked_queue<T>* q, struct plk_item_wrap<T>* wrap = nullptr)
{
	if (wrap == nullptr) {
		wrap = new struct plk_item_wrap<T>();
	}
	else {
		wrap->next = nullptr;
	}
	wrap->prev = q->tail;
	if (q->tail) {   // has item
		q->tail->next = wrap;
	}
	else {  // no item
		q->head = wrap;
	}
	q->tail = wrap;
	++q->size;
	
#ifdef PLK_DEBUG
	dbg_queue_check(q);
#endif // PLK_DEBUG
}

template<typename T>
static inline struct plk_item_wrap<T>* queue_pop(struct plk_linked_queue<T>* q)
{
	struct plk_item_wrap<T>* head = q->head;
	if (head) {   // has item
		--q->size;
		q->head = head->next;
		if (q->head) {   // update newhead->prev 
			q->head->prev = nullptr;
		}
		else{   // empty
			q->tail = nullptr;
		}
		head->prev = nullptr;
		head->next = nullptr;
	}

#ifdef PLK_DEBUG
	dbg_queue_check(q);
#endif // PLK_DEBUG

	return head;
}

template<typename T>
int plk_push(const struct pool_linked<T>* plk, T val)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	// get free node
	struct plk_item_wrap<T>* wrap = queue_pop(&p->queue_free);
	if (wrap == nullptr) {  // no free node, new
		wrap = new struct plk_item_wrap<T>();
	}
	wrap->item = val;
	// add to used queue
	queue_push(&p->queue_used, wrap);
	return 1;
}

template<typename T>
int plk_push_wrap(const struct pool_linked<T>* plk, const struct plk_item_wrap<T>* wrap)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	// add to used queue
	queue_push(&p->queue_used, const_cast<struct plk_item_wrap<T>*>(wrap));
	return 1;
}

template<typename T>
int plk_push_ptr(const struct pool_linked<T>* plk, T* ptr)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	// get free node
	struct plk_item_wrap<T>* wrap = queue_pop(&p->queue_free);
	if (wrap == nullptr) {  // no free node, new
		wrap = new struct plk_item_wrap<T>();
	}
	wrap->item = *ptr;
	// add to used queue
	queue_push(&p->queue_used, wrap);
	return 1;
}

template<typename T>
int plk_pop(const struct pool_linked<T>* plk, T* wrap=nullptr)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	struct plk_item_wrap<T>* item = queue_pop(&p->queue_used);
	if (item) {   // has item
		if (wrap) {
			*wrap = item->item;
		}
		// back to free-queue
		queue_push(&p->queue_free, item);
		return 1;
	}
	return 0;
}

template<typename T>
const struct plk_item_wrap<T>* plk_pop_wrap(const struct pool_linked<T>* plk)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	struct plk_item_wrap<T>* item = queue_pop(&p->queue_used);
	return item;
}

template<typename T>
int plk_free_wrap(const struct pool_linked<T>* plk, const struct plk_item_wrap<T>* wrap)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	queue_push(&p->queue_free, wrap);
	return 1;
}

template<typename T>
int plk_front(const struct pool_linked<T>* plk, T* wrap = nullptr)
{
	struct plk_item_wrap<T>* front = plk->queue_used.head;
	if (front) {
		if (wrap) {
			*wrap = front->item;
		}
		return 1;
	}
	return 0;
}

template<typename T>
inline int plk_size(const struct pool_linked<T>* plk)
{
	return plk->queue_used.size;
}

template<typename T>
inline int plk_empty(const struct pool_linked<T>* plk)
{
	return plk->queue_used.size == 0;
}

template<typename T>
const struct pool_linked<T>* plk_create(int initCap=0)
{
	assert(initCap > -1);
	struct pool_linked<T>* p = new struct pool_linked<T>();
	for (int i = 0; i < initCap; ++i) {
		queue_push(&p->queue_free);
	}
	// init queues for iterator
	p->it.queue_free = &p->queue_free;
	p->it.queue_used = &p->queue_used;
	return p;
}

template<typename T>
void plk_destroy(const struct pool_linked<T>* plk)
{
	struct pool_linked<T>* p = const_cast<struct pool_linked<T>*>(plk);
	struct plk_item_wrap<T>* wrap = nullptr;
	// free queue
	while ( (wrap=queue_pop(&p->queue_free)) != nullptr ) {
		delete wrap;
	}
	// used queue
	while ((wrap = queue_pop(&p->queue_used)) != nullptr) {
		delete wrap;
	}
	delete p;
}


#endif // !POOL_LINKED_H


