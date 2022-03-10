#include "tcp_decode_websocket.h"
#include "tcp_protocol.h"

struct tcp_decode_websocket* decws_new(struct protocol_cfg_websocket* cfg)
{
	struct tcp_decode_websocket* d = new struct tcp_decode_websocket;
	decws_reset(d, cfg);
	return d;
}

void decws_reset(struct tcp_decode_websocket* d, struct protocol_cfg_websocket* cfg)
{

}


// -1: not done   0:has done(succ)  >0: has done(errCode)
int decws_conn_check(struct tcp_decode*dec, char* buf, int bufSize, int& readBytes)
{
	readBytes = 0;
	if(dec->connDone){  // conn has done
		return dec->connRet;
	}
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;

	return -1;
}
