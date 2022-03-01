
#ifndef RING_QUEUE_H
#define RING_QUEUE_H

#include <assert.h>
#include <cstdint>
#include <cstddef>

template<typename T>
struct ring_item_wrap 
{
	T item;
	uint8_t has = 0;
};

template<typename T>
struct ring_queue
{
	int cap;
	int head;
	int tail;
	int size;
	int item_num;
	struct ring_item_wrap<T>* slots;
};

template<typename T>
static int expand_queue(struct ring_queue<T>* q) 
{
	//assert(q->head == q->tail);
	size_t newCap = q->cap * 2;
	struct ring_item_wrap<T>* newSlots = new struct ring_item_wrap<T>[newCap];
	struct ring_item_wrap<T>* oldSlots = q->slots;
	//copy right part
	int oldCap = q->cap;
	int idxNew = 0;
	int head = q->head;
	int itemNum = q->item_num;
	int itemCnt = 0;
	struct ring_item_wrap<T>* itemOld;
	struct ring_item_wrap<T>* itemNew;
	while (itemCnt < itemNum) {
		itemOld = &oldSlots[head];
		if (itemOld->has) {
			itemNew = &newSlots[idxNew];
			itemNew->item = itemOld->item;
			itemNew->has = 1;
			++itemCnt;
		}
		++idxNew;
		if (++head >= oldCap) {
			head = head%oldCap;
		}
	}
	//
	delete[](oldSlots);
	q->slots = newSlots;
	q->head = 0;
	q->tail = q->size;
	q->cap = newCap;
	return 1;
}

template<typename T>
int rq_push(const struct ring_queue<T>* queue, T* val=nullptr)
{
	struct ring_queue<T>* q = const_cast<struct ring_queue<T>*>(queue);
	struct ring_item_wrap<T>* item = &q->slots[q->tail];
	if (val) {
		item->item = *val;
		item->has = 1;
		++q->item_num;
	}
	else {
		item->has = 0;
	}
	++q->size;    // increase item size
	//
	if (++q->tail >= q->cap) {
		q->tail = q->tail%q->cap;
	}
	if (q->tail == q->head) {  // full, expand
		expand_queue(q);
	}
	return 1;
}

template<typename T>
int rq_pop(const struct ring_queue<T>* queue, T* wrap=nullptr)
{
	struct ring_queue<T>* q = const_cast<struct ring_queue<T>*>(queue);
	if (q->head != q->tail) {  // has sth to pop
		struct ring_item_wrap<T>* item = &q->slots[q->head];
		if (++q->head >= q->cap) {
			q->head = q->head%q->cap;
		}
		--q->size;   // decrease queue size
		if (item->has) {  // item is not null
			if (wrap) {
				*wrap = item->item;
			}
			item->has = 0;
			--q->item_num;
			return 1;
		}
	}
	return 0;
}

template<typename T>
int rq_front(const struct ring_queue<T>* q, T* wrap=nullptr)
{
	if (q->head != q->tail) {  // has item
		struct ring_item_wrap<T>* item = &q->slots[q->head];
		if (item->has) {
			if (wrap) {
				*wrap = item->item;
			}
			return 1;
		}
	}
	return 0;
}

template<typename T>
int rq_expand(const struct ring_queue<T>* queue, int newSize)
{
	int dlt = newSize - queue->size;
	assert(dlt > 0);
	struct ring_queue<T>* q = const_cast<struct ring_queue<T>*>(queue);
	if (newSize < q->cap) {
		q->tail = (q->tail + dlt) % q->cap;
	}
	else {
		while (newSize >= q->cap) {
			expand_queue(q);
		}
		q->tail = (q->head + newSize) % q->cap;
	}
	q->size = newSize;
	return 1;
}

template<typename T>
int rq_get(const struct ring_queue<T>* q, int idx, T* wrap=nullptr)
{
	assert(idx > -1 && idx < q->size);
	struct ring_item_wrap<T>* item = &q->slots[(q->head + idx) % q->cap];
	if (item->has) {
		if (wrap) {
			*wrap = item->item;
		}
		return 1;
	}
	return 0;
}

template<typename T>
int rq_set(const struct ring_queue<T>* q, int idx, T* val = nullptr)
{
	assert(idx > -1 && idx < q->size);
	struct ring_item_wrap<T>* item = &q->slots[(q->head + idx) % q->cap];
	int posEmpty = item->has ? 0 : 1;
	if (val) {
		item->item = *val;
		item->has = 1;
		if (posEmpty) {
			struct ring_queue<T>* queue = const_cast<struct ring_queue<T>*>(q);
			++queue->item_num;
		}
	}
	else {
		item->has = 0;
		if (!posEmpty) {
			struct ring_queue<T>* queue = const_cast<struct ring_queue<T>*>(q);
			--queue->item_num;
		}
	}
	return posEmpty;
}

template<typename T>
inline int rq_cap(const struct ring_queue<T>* q)
{
	return q->cap;
}

template<typename T>
inline int rq_size(const struct ring_queue<T>* q)
{
	return q->size;
}

template<typename T>
inline int rq_empty(const struct ring_queue<T>* q)
{
	return q->size == 0;
}

template<typename T>
inline int rq_item_num(const struct ring_queue<T>* q)
{
	return q->item_num;
}

template<typename T>
int rq_clear(const struct ring_queue<T>* queue)
{
	struct ring_queue<T>* q = const_cast<struct ring_queue<T>*>(queue);
	q->head = 0;
	q->tail = 0;
	q->size = 0;
	q->item_num = 0;
	return 1;
}

template<typename T>
const struct ring_queue<T>* rq_create(int initCap)
{
	struct ring_queue<T>* q = new struct ring_queue<T>();
	q->cap = initCap;
	q->head = 0;
	q->tail = 0;
	q->size = 0;
	q->item_num = 0;
	q->slots = new struct ring_item_wrap<T>[q->cap]; 
	return q;
}

template<typename T>
void rq_destroy(const struct ring_queue<T>* queue)
{
	struct ring_queue<T>* q = const_cast<struct ring_queue<T>*>(queue);
	if (q->slots) {
		delete[] q->slots;
		q->slots = nullptr;
	}
	delete q;
}



// for debug
typedef int(*rq_item_compare)(void* pvItem, int has, void* pItem);
#include <vector>
template<typename T>
int rq_dbg_compare(const struct ring_queue<T>* queue, std::vector<T*>& v,
					rq_item_compare fn_compare)
{
	int ret = -1;
	do {
		struct ring_queue<T>* q = const_cast<struct ring_queue<T>*>(queue);
		int sz1 = rq_size(q);
		int sz2 = v.size();
		if (sz1 != sz2) {
			ret = 1;
			break;
		}
		if (sz1 > 0) {
			int tmpRet = 0;
			int vCnt = 0;
			int idx = q->head;
			while (idx != q->tail) {
				struct ring_item_wrap<T>* item = &q->slots[idx];
				T* pVal = v.at(vCnt++);
				if (!fn_compare(pVal, item->has, &item->item)) {
					tmpRet = 2;
					break;
				}
				if (++idx >= q->cap) {
					idx = idx%q->cap;
				}
			}
			if (tmpRet != 0) {
				ret = tmpRet;
				break;
			}
		}
		ret = 0;
	} while (false);
	return ret;
}


#endif // !RING_QUEUE_H




