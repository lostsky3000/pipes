#ifndef TCP_PROTOCOL_H
#define TCP_PROTOCOL_H

#include <cstring>
#include <cstdio>
#include "pps_malloc.h"

#define PPSTCP_PROTOCOL_INVALID -1
#define PPSTCP_PROTOCOL_WEBSOCKET 1

struct protocol_cfg
{
	int type;
};
struct protocol_cfg_websocket
{
	struct protocol_cfg head;
	int uriLen;
	int uriCap;
	char* uri;
};

inline bool protocol_check_cfgtype(const char* name, int* typeCode){
	if(name && strcmp(name,"websocket")==0){
		if(typeCode){
			*typeCode = PPSTCP_PROTOCOL_WEBSOCKET;
		}
		return true;
	}
	return false;
}
inline struct protocol_cfg* protocol_cfg_alloc(int type)
{
	if(type == PPSTCP_PROTOCOL_WEBSOCKET){
		struct protocol_cfg_websocket* cfg = new struct protocol_cfg_websocket;
		cfg->head.type = PPSTCP_PROTOCOL_WEBSOCKET;
		cfg->uri = nullptr;
		cfg->uriCap = 0;
		cfg->uriLen = 0;
		return (struct protocol_cfg*)cfg;
	}
	return nullptr;
}
inline struct protocol_cfg* protocol_cfg_clone(struct protocol_cfg* icfg)
{
	if(icfg->type == PPSTCP_PROTOCOL_WEBSOCKET){
		struct protocol_cfg_websocket* cfgOld = (struct protocol_cfg_websocket*)icfg;
		struct protocol_cfg_websocket* cfgNew = new struct protocol_cfg_websocket;
		cfgNew->head.type = PPSTCP_PROTOCOL_WEBSOCKET;
		if(cfgOld->uri){
			int uriLen = cfgOld->uriLen;
			int uriCap = cfgOld->uriCap;
			cfgNew->uri = new char[uriCap];
			memcpy(cfgNew->uri, cfgOld->uri, uriLen);
			cfgNew->uri[uriLen] = '\0';
			cfgNew->uriLen = uriLen;
			cfgNew->uriCap = uriCap;
		}else{
			cfgNew->uri = nullptr;
			cfgNew->uriLen = 0;
			cfgNew->uriCap = 0;
		}
		return (struct protocol_cfg*)cfgNew;
	}
	return nullptr;
}
inline void protocol_cfg_free(struct protocol_cfg* icfg)
{
	if(icfg->type == PPSTCP_PROTOCOL_WEBSOCKET){
		struct protocol_cfg_websocket* cfg = (struct protocol_cfg_websocket*)icfg;
		if(cfg->uri){
			delete[] cfg->uri;
			cfg->uri = nullptr;
			cfg->uriLen = 0;
			cfg->uriCap = 0;
		}
		delete cfg;
	}
}
inline int protocol_cfg_add_item_str(struct protocol_cfg* icfg, const char* key, const char* val,
	char* errBuf, int errBufSize)
{
	if(icfg->type == PPSTCP_PROTOCOL_WEBSOCKET){
		struct protocol_cfg_websocket* cfg = (struct protocol_cfg_websocket*)icfg;
		if(strcmp(key, "uri") == 0){
			int strLen = strlen(val);
			if(cfg->uri && cfg->uriCap <= strLen){
				delete[] cfg->uri;
				cfg->uri = nullptr;
			}
			if(cfg->uri == nullptr){
				cfg->uriCap = strLen + 1;
				cfg->uri = new char[cfg->uriCap];
			}
			memcpy(cfg->uri, val, strLen);
			cfg->uri[strLen] = '\0';
			cfg->uriLen = strLen;
			return 1;
		}else if(strcmp(key, "type") == 0){
		
		}else{
			sprintf(errBuf, "unknown protocol cfg field for websocket: %s", key);
			return -1;
		}
	}
	return 0;
}
inline int protocol_cfg_add_item_int(struct protocol_cfg* icfg, const char* key, int val,
	char* errBuf, int errBufSize)
{
	if (icfg->type == PPSTCP_PROTOCOL_WEBSOCKET) {
		struct protocol_cfg_websocket* cfg = (struct protocol_cfg_websocket*)icfg;
		sprintf(errBuf, "unknown protocol cfg field for websocket: %s", key);
		return -1;
	}
	return 0;
}

inline bool protocol_cfg_whole_check(struct protocol_cfg* icfg, char* errBuf, int errBufSize)
{
	if(icfg->type == PPSTCP_PROTOCOL_WEBSOCKET){
		struct protocol_cfg_websocket* cfg = (struct protocol_cfg_websocket*)icfg;
		if(cfg->uri){
			return true;
		}else{
			sprintf(errBuf, "uri not speficy for websocket cfg");
			return false;
		}
	}
	sprintf(errBuf, "unknown protocol type: %d", icfg->type);
	return false;
}

#endif // !PPS_TCP_PROTOCOL_H
