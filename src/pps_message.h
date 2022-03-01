#ifndef PPS_MESSAGE_H
#define PPS_MESSAGE_H

#include <cstdint>
#include "pps_malloc.h"

struct pps_message
{
	uint32_t idx_pair;
	uint32_t from_cnt;
	uint32_t to_cnt;
	int32_t session;
	//
	uint32_t size;
	void* data;
};

#define MTYPE_USER_START 1
#define MTYPE_USER_EXIT 2
#define MTYPE_SVC_GONE 3
#define MTYPE_SVC_SELFMSG 4

#define MTYPE_CUSTOM_BEGIN 10

inline uint32_t idxpair_encode(uint32_t from, uint32_t to) {
	return (from << 16) | to;
}
inline uint32_t idxpair_decode(uint32_t idxPair, uint32_t* from=nullptr) {
	if (from) {
		*from = idxPair >> 16;
	}
	return idxPair & 0xFFFF;
}

#define MSGTYPE_SHIFT_BITS 5
inline uint32_t msg_type(uint32_t szCombine)
{
	return szCombine >> (32 - MSGTYPE_SHIFT_BITS);
}
inline uint32_t msg_size_real(uint32_t szCombine)
{
	const uint32_t mask = (UINT32_MAX << MSGTYPE_SHIFT_BITS) >> MSGTYPE_SHIFT_BITS;
	return szCombine & mask;
}
inline uint32_t msg_size_combine(uint32_t msgType, uint32_t szReal)
{
	return (msgType << (32 - MSGTYPE_SHIFT_BITS)) | szReal;
}

inline void msg_gen_svc_exit(struct pps_message* m) {
	m->data = nullptr;
	m->size = msg_size_combine(MTYPE_USER_EXIT, 0);
}

#endif // !PPS_MESSAGE_H

