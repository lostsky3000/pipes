#ifndef LPPS_ADAPTER_H
#define LPPS_ADAPTER_H
#include <cstdint>
#include <cstring>
#include "pps_message.h"
#include "pps_worker.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#define LPPS_SVC_CTX "LPPS_SVC_CTX"
#define LPPS_OPEN_C_LIB "LPPS_OPEN_C_LIB"
#define LPPS_OPEN_C_SOCK_LIB "LPPS_OPEN_C_SOCK_LIB"


#define LPPS_MTYPE_CALL_NO_RET MTYPE_CUSTOM_BEGIN

#define LPPS_MTYPE_LUABEGIN MTYPE_CUSTOM_BEGIN + 1
#define LPPS_MTYPE_STR LPPS_MTYPE_LUABEGIN
#define LPPS_MTYPE_NUM LPPS_MTYPE_LUABEGIN + 1
#define LPPS_MTYPE_LUA LPPS_MTYPE_LUABEGIN + 2

struct read_tmp
{
	int total;
	lua_State* L;
	struct worker_tmp_buf* tmpBuf;
};

inline int lpps_read_msg(int total, int cur, char* buf, int size, void*ud)
{
	struct read_tmp* tmp = (struct read_tmp*)ud;
	if (total > 0) {    // not empty
		struct worker_tmp_buf* tmpBuf = tmp->tmpBuf;
		if (cur == 0) {// 1st part
			tmp->total = total;
			if (size == total) {   // only one copy
				lua_pushlstring(tmp->L, buf, size);
				return 1;
			}
			if (tmpBuf->cap < total) {   // expand
				tmpBuf->buf = (char*)pps_realloc(tmpBuf->buf, total);
				tmpBuf->cap = total;
			}
		}
		memcpy(tmpBuf->buf + cur, buf, size);
		if (cur + size >= total) {	//  last part
			lua_pushlstring(tmp->L, tmpBuf->buf, total);
		}
	}
	else {   // empty
		tmp->total = 0;
		lua_pushstring(tmp->L, "");
	}
	return 1;
}


struct pipes;
struct pps_service;

struct lpps_svc_ctx
{
	bool exit;
	uint32_t mem;
	uint32_t szParam;
	lua_Integer luaCb;
	lua_Integer timerCb;
	lua_Integer netCb;
	struct pps_service* svc;
	lua_State* L;
	char* src;
	void* param;
	int64_t svcId;
};

int64_t lpps_newservice(const char* srcName, void*param, 
			uint32_t szParam, struct pipes*pipes, struct pps_service* caller, bool isExclusive);

int lpps_onboot(void* ud, struct pipes* pipes);


int lpps_destroy_msg(struct pps_message*m, void* adapter);

//
inline int64_t luasvcid_encode(uint32_t svcIdx, uint32_t svcCnt)
{
	int64_t id = svcIdx;
	return (id << 32) | svcCnt;
}
inline uint32_t luasvcid_decode(int64_t luaId, uint32_t* svcCnt)
{
	*svcCnt = luaId & 0xffffffff;
	return luaId >> 32;
}

#endif // !LPPS_BOOT_H

