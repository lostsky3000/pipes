#ifndef TCP_DECODE_WEBSOCKET_H
#define TCP_DECODE_WEBSOCKET_H

#include "tcp_decode.h"
#include "dec_http_head.h"

struct tcp_decode_websocket
{
	struct tcp_decode head;
	//
	int uriCap;
	int uriLen;
	char* uri;
	//
	int secKeySaltPos;
	int bufSecKeyCap;
	char* bufSecKey;
	//
	int bufSecKeyRspLen;
	char* bufSecKeyRsp;
	//
	struct dec_http_head* decHttpHead;
	// pack dec
	int state;
};

struct protocol_cfg_websocket;
struct tcp_decode_websocket* decws_new(struct protocol_cfg_websocket* cfg);
void decws_reset(struct tcp_decode_websocket* d, struct protocol_cfg_websocket* cfg);
void decws_destroy(struct tcp_decode_websocket* d);

//
int decws_conn_check(struct tcp_decode*d, char* buf, int bufSize, int& readBytes);
int decws_conn_rsp(struct tcp_decode*d, void* ud, FN_DECODE_CONN_RSP_IMPL impl);
int decws_pack_check(struct tcp_decode*d, uint8_t* buf, int bufSize, int& checkedBytes);

#endif // !TCP_DECODE_WEBSOCKET_H

