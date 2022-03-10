#ifndef TCP_DECODE_WEBSOCKET_H
#define TCP_DECODE_WEBSOCKET_H
#include "tcp_decode.h"

struct tcp_decode_websocket
{
	struct tcp_decode head;

};

struct protocol_cfg_websocket;
struct tcp_decode_websocket* decws_new(struct protocol_cfg_websocket* cfg);
void decws_reset(struct tcp_decode_websocket* d, struct protocol_cfg_websocket* cfg);

int decws_conn_check(struct tcp_decode*d, char* buf, int bufSize, int& readBytes);

#endif // !TCP_DECODE_WEBSOCKET_H

