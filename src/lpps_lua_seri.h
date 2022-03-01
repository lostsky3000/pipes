#ifndef LPPS_LUA_SERI_H
#define LPPS_LUA_SERI_H

#include <cstdlib>
extern "C" {
#include "lua.h"
}

typedef void*(*fn_lua_seri_realloc)(
	void* ptr, size_t szOldData,
	size_t szNewPrefer, size_t szNewMin, size_t* szNewActual);

typedef void(*fn_lua_deseri_ptr_iter)(void* ptr, void* ud);

struct lpps_lua_seri_ctx {
	size_t bytesPacked;
	size_t bufCap;
	void* buf;
	fn_lua_seri_realloc fnRealloc;
};

void* lua_seri_pack(
	lua_State* L,
	int from,
	struct lpps_lua_seri_ctx* seriCtx);


int lua_seri_unpack(lua_State*L, void*buf, size_t szBuf, 
		fn_lua_deseri_ptr_iter itPtr, void* ud);


#endif // !LPPS_LUA_SERI_H






