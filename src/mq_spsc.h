#ifndef MQ_SPSC_H
#define MQ_SPSC_H
#include <atomic>
#include <cstdint>

template<typename T>
struct spsc_block
{
	uint32_t idx;
	uint32_t cap;
	uint32_t idxRead;
	uint32_t idxWrite;
	T* arr;
	struct spsc_block<T>* next;	
};

template<typename T>
struct spsc_block_queue
{
	uint32_t size;
	struct spsc_block<T>* head;
	struct spsc_block<T>* tail;
};

template<typename T>
struct mq_spsc
{
	uint32_t blockCap;
	std::atomic<uint32_t> readable;
	std::atomic<uint32_t> readBlockIdx;
	uint32_t writeBlockIdx;
	struct spsc_block<T>* blockRead;
	struct spsc_block<T>* blockWrite;
	struct spsc_block_queue<T> queReading;
	struct spsc_block_queue<T> queFree;
};

template<typename T>
static void que_clear(struct spsc_block_queue<T>* q)
{
	q->size = 0;
	q->head = nullptr;
	q->tail = nullptr;
}

template<typename T>
static void que_push(struct spsc_block_queue<T>* q, struct spsc_block<T>* b)
{
	b->next = nullptr;
	if (q->tail) {
		q->tail->next = b;
		q->tail = b;
	}
	else {
		q->head = b;
		q->tail = b;
	}
	++q->size;
}

template<typename T>
static struct spsc_block<T>* que_pop(struct spsc_block_queue<T>* q)	
{
	struct spsc_block<T>* b = q->head;
	if (b) {
		q->head = b->next;
		b->next = nullptr;
		if (q->head == nullptr) {
			q->tail = nullptr;
		}
		--q->size;
	}
	return b;
}
template<typename T>
static struct spsc_block<T>* que_front(struct spsc_block_queue<T>* q)	
{
	return q->head;
}

template<typename T>
static void block_reset(struct spsc_block<T>* b, uint32_t idx)
{
	b->idx = idx;
	b->idxRead = 0;
	b->idxWrite = 0;
	b->next = nullptr;
}
// only called by spsc_pushxxx and spsc_create()
template<typename T>
static struct spsc_block<T>* block_new(uint32_t cap, uint32_t idx)
{
	struct spsc_block<T>* b = new struct spsc_block<T>();
	b->cap = cap;
	b->arr = new T[cap];
	block_reset(b, idx);
	return b;
}

template<typename T>
int spsc_push_ptr(struct mq_spsc<T>*q, T* pVal)	
{
	//recycle block that has read
	uint32_t readBlockIdx = q->readBlockIdx.load(std::memory_order_acquire);
	struct spsc_block<T>* b;
	while( (b=que_front(&q->queReading)) ){
		if(b->idx == readBlockIdx){   // this block is inreading
			break;
		}
		//recycle block
		b = que_pop(&q->queReading);
		que_push(&q->queFree, b);
	}
	// 
	b = q->blockWrite;
	uint32_t& idxWrite = b->idxWrite;
	if(idxWrite < b->cap){   // can write in curBlock
		b->arr[idxWrite++] = *pVal;
	}else{   //  curBlock is full, new block
		b = que_pop(&q->queFree);
		if(b){
			block_reset(b, ++q->writeBlockIdx);
		}else{
			b = block_new<T>(q->blockCap, ++q->writeBlockIdx); 
		}
		que_push(&q->queReading, b);
		q->blockWrite = b;
		b->arr[b->idxWrite++] = *pVal;
	}
	q->readable.fetch_add(1, std::memory_order_release);
	return 1;
}
template<typename T>
int spsc_push(struct mq_spsc<T>*q, T val)
{
	return spsc_push_ptr(q, &val);	
}

template<typename T>
static void ensure_pop(struct mq_spsc<T>*q, T* wrap)
{
	struct spsc_block<T>* b = q->blockRead;
	if(b->idxRead >= b->cap){   // cur block read done, change to next
		b = b->next;
		b->idxRead = 0;
		q->blockRead = b;
		q->readBlockIdx.fetch_add(1, std::memory_order_release);
	}
	*wrap = b->arr[b->idxRead++];
}

template<typename T>
int spsc_pop(struct mq_spsc<T>*q, T* wrap, uint32_t* pLeft=nullptr)
{
	uint32_t readable = q->readable.load(std::memory_order_acquire);
	if(readable <= 0){   // no data
		return 0;
	}
	ensure_pop(q, wrap);
	if(pLeft){
		*pLeft = readable - 1;
	}
	q->readable.fetch_sub(1, std::memory_order_relaxed);
	return 1;
}

template<typename T>
uint32_t spsc_size(struct mq_spsc<T>* q)
{
	return q->readable.load(std::memory_order_acquire);
}

template<typename T>
struct mq_spsc<T>* spsc_create(uint32_t blockSize)
{
	struct mq_spsc<T>* q = new struct mq_spsc<T>();
	struct spsc_block<T>* b = block_new<T>(blockSize, 0);
	//
	q->blockCap = blockSize;
	q->blockRead = b;
	q->blockWrite = b;
	que_clear(&q->queReading);
	que_clear(&q->queFree);
	que_push(&q->queReading, b);
	q->writeBlockIdx = 0;
	q->readBlockIdx.store(0, std::memory_order_release);
	q->readable.store(0, std::memory_order_release);
	return q;
}

template<typename T>
void spsc_destroy(struct mq_spsc<T>* q)
{
	
	delete q;
}

template<typename T>
void spsc_read_thread_init(struct mq_spsc<T>*q)	
{
	q->readable.load(std::memory_order_acquire);
	uint32_t ui = q->blockRead->idxRead;
}
template<typename T>
void spsc_write_thread_init(struct mq_spsc<T>*q)	
{
	q->readable.load(std::memory_order_acquire);
	uint32_t ui = q->blockWrite->idxWrite;
	ui = q->writeBlockIdx;
}

#endif // !MQ_SPSC_H
