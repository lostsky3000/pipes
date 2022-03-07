#ifndef TCP_DECODE_H
#define TCP_DECODE_H

#include <cstdint>

struct tcp_decode
{
	int16_t type;
	int16_t connHeadDone;
};

struct protocol_cfg;
struct tcp_decode* decode_new(struct protocol_cfg* cfg);

#endif // !PROTOCOL_DECODE_H

