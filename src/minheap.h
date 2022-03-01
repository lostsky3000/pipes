#ifndef MINHEAP_H
#define MINHEAP_H

#include <cstdint>

template<typename T>
struct minheap_node
{
	uint32_t idx;
	T udata;
};

template<typename T>
struct minheap_queue
{
	uint32_t node_cap;
	uint32_t node_num;
	struct minheap_node<T>* nodes;
	int(*fn_compare)(T, T);
};

template<typename T>
struct minheap_queue<T>* minheap_create(
	uint32_t nodeCap, 
	int(*fnCompare)(T, T))
{
	struct minheap_queue<T>* queue = new struct minheap_queue<T>;
	queue->fn_compare = fnCompare;
	queue->node_cap = nodeCap;
	queue->node_num = 0;
	queue->nodes = new struct minheap_node<T>[nodeCap];
	for (uint32_t i = 0; i < nodeCap; ++i)
	{
		struct minheap_node<T>* node = &queue->nodes[i];
		node->idx = i;
	}
	return queue;
}

template<typename T>
void minheap_destroy(struct minheap_queue<T>* q)
{
	delete[] q->nodes ;
	delete q ;
}

template<typename T>
static void shift_up(struct minheap_queue<T>* queue)
{
	struct minheap_node<T>* nodes = queue->nodes;
	uint32_t id = queue->node_num;
	int(*fnCompare)(T, T) = queue->fn_compare;
	T udata;
	while (id > 1)  // has not arrive root yet
	{
		uint32_t upId = id >> 1;
		if (fnCompare(nodes[id - 1].udata, nodes[upId - 1].udata) < 0)  // smaller than up, swap
		{
			udata = nodes[id - 1].udata;
			nodes[id - 1].udata = nodes[upId - 1].udata;
			nodes[upId - 1].udata = udata;
			id = upId;
		}
		else  //stop
		{
			break;
		}
	}
}

template<typename T>
static void shift_down(struct minheap_queue<T>* queue)
{
	struct minheap_node<T>* nodes = queue->nodes;
	int(*fnCompare)(T, T) = queue->fn_compare;
	uint32_t id = 1;	
	uint32_t downId;
	T udata;
	while ((downId = id << 1) <= queue->node_num)
	{
		if (downId < queue->node_num) {
			//has right-child, compare
			if(fnCompare(nodes[downId - 1].udata, nodes[downId].udata) > 0) //bigger than right, choose smaller
			{
				++downId;
			}
		}
		if (fnCompare(nodes[downId - 1].udata, nodes[id - 1].udata) < 0) // bigger than child, swap
		{
			udata = nodes[downId - 1].udata;
			nodes[downId - 1].udata = nodes[id - 1].udata;
			nodes[id - 1].udata = udata;
			id = downId;
		}
		else
		{
			break;
		}
	}
}

template<typename T>
void minheap_add(T udata, struct minheap_queue<T>* queue)
{
	if (queue->node_num >= queue->node_cap) //queue is full
	{
		queue->node_cap *= 2;
		struct minheap_node<T>* oldNodes = queue->nodes;
		//
		struct minheap_node<T>* newNodes = new struct minheap_node<T>[queue->node_cap];
		queue->nodes = newNodes;
		//
		for (uint32_t i=0; i<queue->node_num; ++i) {
			newNodes[i] = oldNodes[i];
		}
		//
		delete[] oldNodes;
	}
	queue->nodes[queue->node_num].udata = udata;
	++queue->node_num;
	//
	shift_up(queue);
}

template<typename T>
T minheap_get(struct minheap_queue<T>* queue)
{
	if (queue->node_num < 1) // empty
		{
			return nullptr;
		}
	return queue->nodes[0].udata;
}

template<typename T>
T minheap_pop(struct minheap_queue<T>* queue)
{
	if (queue->node_num < 1) // empty
		{
			return nullptr;
		}
	T udata = queue->nodes[0].udata;
	//resort
	if(--queue->node_num > 0)  // still has node, resort
	{
		queue->nodes[0].udata = queue->nodes[queue->node_num].udata;
		shift_down(queue);
	}
	return udata;
}

template<typename T>
uint32_t minheap_size(struct minheap_queue<T>* queue)
{
	return queue->node_num;
}

#endif // !MINHEAP_H



