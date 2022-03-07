#ifndef TCP_DECODE_WEBSOCKET_H
#define TCP_DECODE_WEBSOCKET_H
#include "tcp_decode.h"

struct tcp_decode_websocket
{
	struct tcp_decode head;
};

struct protocol_cfg_websocket;
struct tcp_decode_websocket* decws_new(struct protocol_cfg_websocket* cfg);

#endif // !TCP_DECODE_WEBSOCKET_H

