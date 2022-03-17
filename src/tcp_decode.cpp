#include "tcp_decode.h"
#include "tcp_decode_websocket.h"
#include "tcp_protocol.h"
#include <cassert>

static inline void reset_decode(struct tcp_decode* d, struct protocol_cfg* cfg)
{
	d->protocol = cfg->type;
	d->state = TCPDEC_STATE_CONN_CHECKING;
	d->errCode = 0;
}

struct tcp_decode* decode_new(struct protocol_cfg* cfg)
{
	struct tcp_decode* d = nullptr;
	if(cfg->type == PPSTCP_PROTOCOL_WEBSOCKET){
		struct tcp_decode_websocket* dec = decws_new((struct protocol_cfg_websocket*)cfg);
		d = (struct tcp_decode*)dec;
		d->cbConnCheck = decws_conn_check;
		d->cbConnRsp = decws_conn_rsp;
		d->cbPackHead = decws_pack_head;
		d->cbPackDecBody = decws_pack_dec_body;
		d->cbPackReset = decws_pack_reset;
	}
	assert(d);
	reset_decode(d, cfg);
	
	return d;
}
void decode_reset(struct tcp_decode* d, struct protocol_cfg* cfg)
{
	assert(d->protocol == cfg->type);
	if(d->protocol == PPSTCP_PROTOCOL_WEBSOCKET){
		decws_reset((struct tcp_decode_websocket*)d, (struct protocol_cfg_websocket*)cfg);
	}
	reset_decode(d, cfg);
}
int decode_destroy(struct tcp_decode* d)
{
	if(d->protocol == PPSTCP_PROTOCOL_WEBSOCKET){
		decws_destroy((struct tcp_decode_websocket*)d);
		return 1;
	}
	return 0;
}


//
int decsep_check(struct dec_sep* d, const char* buf, int bufSize, int& readBytes)
{
	readBytes = 0;
	if (d->chMatchCnt == d->sepLen) {
		return 1;
	}
	int seek = 0;
	while (seek < bufSize) {
		if (buf[seek] == d->sep[d->chMatchCnt]) {
			++d->chMatchCnt;
		} else {
			d->chMatchCnt = 0;
		}
		++seek;
		if (d->chMatchCnt == d->sepLen) {   // match
			readBytes += seek;
			d->seekedTotal += seek;
			return 1;
		}
	}
	readBytes += seek;
	d->seekedTotal += seek;
	return 0;
}

