
#ifndef PPS_MALLOC_H
#define PPS_MALLOC_H

#include <cstdlib>

inline void* pps_malloc(size_t sz)
{
	void* ptr = malloc(sz);
	return ptr;
}

inline void pps_free(void* ptr)
{
	free(ptr);
}

inline void* pps_realloc(void* ptr, size_t sz) {
	return realloc(ptr, sz);
}

#endif // !PPS_MALLOC_H



