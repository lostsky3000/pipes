#include "tcp_decode.h"
#include "tcp_decode_websocket.h"
#include "tcp_protocol.h"


struct tcp_decode* decode_new(struct protocol_cfg* cfg)
{
	if(cfg->type == PPSTCP_PROTOCOL_WEBSOCKET){
		struct tcp_decode_websocket* dec = decws_new((struct protocol_cfg_websocket*)cfg);
		return (struct tcp_decode*)dec;
	}
	return nullptr;
}

