#include "lpps_adapter.h"

#include "pipes.h"
#include "pps_config.h"
#include "pps_service.h"
#include "pps_api_lua.h"
#include "lpps_lua_seri.h"
#include "pps_timer.h"
#include "pps_net.h"
#include "pps_api_lua_socket.h"
#include "pps_worker.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#include <cstring>

#include <cstdio>  // debug

static inline void param_destroy(struct lpps_svc_ctx* lctx);
static inline void lctx_destroy(struct lpps_svc_ctx* lctx);
static void* lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
static int on_lua_svc_start(struct lpps_svc_ctx* lctx, struct pps_service*s, 
	int* errCode, char** errMsg);

static int on_lua_msg(struct pps_message*m, uint32_t msgType, struct lpps_svc_ctx* lctx)
{
	lua_State* L = lctx->L;
	int fnType = lua_rawgeti(L, LUA_REGISTRYINDEX, lctx->luaCb);
	if (fnType == LUA_TFUNCTION) // has reg msg cb
	{   // from, session, type, data, sz
		int argNum = 3;
		uint32_t idxFrom;
		idxpair_decode(m->idx_pair, &idxFrom);
		lua_pushnumber(L, luasvcid_encode(idxFrom, m->from_cnt));
		lua_pushinteger(L, m->session);
		lua_pushinteger(L, msgType);
		if (m->data) {
			argNum = 5;
			lua_pushlightuserdata(L, m->data);
			lua_pushinteger(L, m->size);
		}
		int callRet = lua_pcall(L, argNum, 1, 0);
		if (callRet == LUA_OK) { // call succ
			lua_pop(L, 1);  // pop the ret
		}
		else { // call error
			const char* err = lua_tostring(L, -1);
			printf("lua_pcall err: %s\n", err);  // debug
			lua_pop(L, 1);  // pop the err
		}
	}
	else {
		lua_pop(L, 1); // pop the rawi
	}
	return 0;
}


static void on_net_msg(struct net_msg*m, void* adapter, struct pps_service* s, struct pps_worker* wk)
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)adapter;
	if (lctx->exit) {
		return;
	}
	lua_State* L = lctx->L;
	int fnType = lua_rawgeti(L, LUA_REGISTRYINDEX, lctx->netCb);
	if (fnType == LUA_TFUNCTION) // has reg msg cb
		{
			int argNum = 0;
			switch (m->cmd)
			{
			case NETCMD_READ_WAIT:
				{
					struct tcp_read_wait_ret* ret = (struct tcp_read_wait_ret*)m->buf; 
					lua_pushinteger(L, NETCMD_READ_WAIT);
					struct read_arg arg;
					arg.isNewRead = false;
					arg.session = ret->session;
					arg.srcIdx = s->svcIdx;
					arg.srcCnt = s->svcCntLocal;
					struct read_tmp tmp;
					tmp.L = L;
					tmp.tmpBuf = &lctx->svc->curWorker->tmpBuf;
					arg.ud = &tmp;
					arg.cb = lpps_read_msg;
					int trunc;
					int retRead = net_tcp_read(s->pipes, ret->sockId.idx, ret->sockId.cnt, &arg, &trunc);
					if(retRead > 0){  // has read sth
						lua_pushinteger(L, ret->session);
						lua_pushinteger(L, tmp.total);
						if (retRead == 1) {	// normal read
							lua_pushboolean(L, true);
							lua_pushboolean(L, trunc);
							argNum = 6;
						} else {   // last read
							lua_pushboolean(L, false);
							argNum = 5;
						}
					}else if(retRead == 0){    // continue read wait
						lua_pop(L, 1);
						return;
					}else{
						if (retRead != -1) {  // invalid call, warning?
							lua_pop(L, 1);
							return;
						}
						// sock has gone
						lua_pushboolean(L, false);
						lua_pushinteger(L, ret->session);
						argNum = 3;
					}
					break;
				}
			case NETCMD_TCP_CONNIN:
				{
					struct tcp_conn_in* mIn = (struct tcp_conn_in*)m->buf;
					int32_t listenSockIdx = mIn->sockIdParent.idx;
					uint32_t listenSockCnt = mIn->sockIdParent.cnt;
					if (!net_is_listen_valid(lctx->svc->pipes, listenSockIdx, listenSockCnt)) { // listen sock has closed
						return;
					}
					lua_pushinteger(L, NETCMD_TCP_CONNIN);
					lua_pushinteger(L, listenSockIdx);
					lua_pushinteger(L, listenSockCnt);
					lua_pushinteger(L, mIn->sockId.idx);
					lua_pushinteger(L, mIn->sockId.cnt);
					argNum = 5;
					break;
				}
			case NETCMD_TCP_CONNECT:
				{
					struct tcp_conn_ret* connRet = (struct tcp_conn_ret*)m->buf;
					lua_pushinteger(L, NETCMD_TCP_CONNECT);
					lua_pushinteger(L, m->session);
					lua_pushinteger(L, connRet->ret);
					if (connRet->ret == 0) {    // conn succ
						lua_pushinteger(L, connRet->sockId.idx);
						lua_pushinteger(L, connRet->sockId.cnt);
						argNum = 5;
					} else {  // conn failed
						
						argNum = 3;
					}
					break;
				}
			case NETCMD_TCP_LISTEN:
				{
					struct tcp_listen_ret* retListen = (struct tcp_listen_ret*)m->buf;
					lua_pushinteger(L, NETCMD_TCP_LISTEN);
					lua_pushinteger(L, m->session);
					if (retListen->ret == 0) {  // succ
						lua_pushboolean(L, true);
						lua_pushinteger(L, retListen->sockId.idx);
						lua_pushinteger(L, retListen->sockId.cnt);
						argNum = 5;
					}
					else {  // failed
						lua_pushboolean(L, false);
						lua_pushinteger(L, retListen->ret);
						argNum = 4;
					}
					break;
				}
			default:
				{	// warning
					// ...
					return;
				}
			}
			int callRet = lua_pcall(L, argNum, 0, 0);
			if (callRet == LUA_OK) {
				 // call succ
			}
			else {
				 // call error
			    const char* err = lua_tostring(L, 1);
				lua_pop(L, 1);      // pop the err
			}
		}
	else {
		lua_pop(L, 1);     // pop the rawi
	}
}

static void on_timer_msg(struct timer_msg*m, void* adapter, struct pps_service* s)
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)adapter;
	if (lctx->exit) {
		return;
	}
	//printf("recv timer msg, session=%d\n", m->session);   // debug
	lua_State* L = lctx->L;
	int fnType = lua_rawgeti(L, LUA_REGISTRYINDEX, lctx->timerCb);
	if (fnType == LUA_TFUNCTION) // has reg msg cb
		{	// session
			lua_pushinteger(L, m->session);
			lua_pushinteger(L, m->repeat);
			lua_pushnumber(L, m->expiration);   // debug
			int callRet = lua_pcall(L, 3, 0, 0);
			if (callRet == LUA_OK) { // call succ
			  
			}
			else { // call error
			    const char* err = lua_tostring(L, 1);
				lua_pop(L, 1);     // pop the err
			}
		}
	else {
		lua_pop(L, 1);    // pop the rawi
	}
}

static void on_svc_msg(struct pps_message*m, void* adapter, struct pps_service* s)
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)adapter;
	uint32_t mType = msg_type(m->size);
	if (mType >= MTYPE_CUSTOM_BEGIN || mType == MTYPE_SVC_GONE || mType == MTYPE_SVC_SELFMSG) {
		if (!lctx->exit) {
			on_lua_msg(m, mType, lctx);
		}
		else {  // exit, destroy msg
			lpps_destroy_msg(m, lctx);
		}
	}
	/*
	else if (mType == MTYPE_NET) {
		if (!lctx->exit) {
			on_net_msg(m, mType, lctx);
		}
		else {
			lpps_destroy_msg(m, lctx);
		}
	}*/
	else if (mType == MTYPE_USER_START) {
		lctx->svc = s;
		int err = 0;
		char* errMsg = nullptr;
		if (!on_lua_svc_start(lctx, s, &err, &errMsg)) { // start lua failed
			if(errMsg) {
				printf("luaSvcStart err: %s\n", errMsg);
			}
			svc_exit(s);
		}
	}
	else if (mType == MTYPE_USER_EXIT) {
		printf("svc exit\n");
		lctx_destroy(lctx);
	}else {
		
	}
}

static inline void on_lua_deseri_ptr(void* ptr, void* ud) 
{
	pps_free(ptr);
}

int lpps_destroy_msg(struct pps_message*m, void* adapter) 
{
	if (m->data) {
		//uint32_t type = msg_type(m->size);
		struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)adapter;
		uint32_t sz = msg_size_real(m->size);
		if (sz > 0) {
			//printf("lpps destroy msg, sz=%d\n", sz);  // debug
			/*
			int argsNum = lua_seri_unpack(lctx->L, m->data, sz, on_lua_deseri_ptr, adapter);
			if (argsNum > 0) {
				lua_pop(lctx->L, argsNum);
			}*/
		}
		pps_free(m->data);
		m->data = nullptr;
		return 1;
	}
	return 0;
}

int lpps_onboot(void* ud, struct pipes* pipes)
{
	printf("lpps_onboot called\n");
	//
	int64_t id = lpps_newservice(pipes->config->boot_service, nullptr, 0, pipes, nullptr);
	assert(id >= 0);
	return 1;	
}
int64_t lpps_newservice(const char* srcName, void*param, uint32_t szParam, 
	struct pipes* pipes, struct pps_service* caller)
{
	struct lpps_svc_ctx* lctx = new struct lpps_svc_ctx;
	lctx->L = nullptr;
	lctx->mem = 0;
	lctx->luaCb = 0;
	lctx->timerCb = 0;
	lctx->netCb = 0;
	lctx->exit = false;
	lctx->param = param;
	lctx->szParam = szParam;
	size_t szSrc = strlen(srcName);
	lctx->src = (char*)pps_malloc(szSrc + 1);
	strcpy(lctx->src, srcName);
	//
	struct pps_service_ud svcUd;
	svcUd.cbOnMsg = on_svc_msg;
	svcUd.cbOnTimerMsg = on_timer_msg;
	svcUd.cbOnNetMsg = on_net_msg;
	svcUd.cbDeMsg = lpps_destroy_msg;
	svcUd.userData = lctx;
	uint32_t svcCnt = 0;
	int32_t svcIdx = svc_newservice(pipes, &svcUd, &svcCnt, caller);
	if (svcIdx < 0) {   // get service failed
		lctx_destroy(lctx);
		return -1;
	}
	lctx->svcId = luasvcid_encode(svcIdx, svcCnt);
	return lctx->svcId;
}

static int on_lua_svc_start(struct lpps_svc_ctx* lctx, struct pps_service* s, 
	int* errCode, char** errMsg)
{
	int ret = -1;
	lua_State* L = nullptr;
	do {
		struct pps_config* cfg = s->pipes->config;
		// create luaState
		L = lua_newstate(lua_alloc, lctx);
		lctx->L = L;
		//L = luaL_newstate();
		// load std libs
		luaL_openlibs(L);
		// set lctx
		lua_pushlightuserdata(L, lctx);
		lua_setfield(L, LUA_REGISTRYINDEX, LPPS_SVC_CTX);
		// set search path
		lua_pushstring(L, cfg->lua_path);
		lua_setglobal(L, "LUA_PATH");
		lua_pushstring(L, cfg->lua_cpath);
		lua_setglobal(L, "LUA_CPATH");
		lua_pushstring(L, lctx->src);
		lua_setglobal(L, "SRC_PATH");
		// set apiLib
		lua_pushcfunction(L, luapps_api_openlib);
		lua_setglobal(L, LPPS_OPEN_C_LIB);
		lua_pushcfunction(L, luapps_api_socket_openlib);
		lua_setglobal(L, LPPS_OPEN_C_SOCK_LIB);
		// load src loader
		int r = luaL_loadfile(L, "./lualib/pps_svc_loader.lua");
		if (r != LUA_OK) {
			ret = -2;
			break;
		}
		// set param
		int argNum = 0;
		if (lctx->param) {   // has param
			argNum = 2;
			lua_pushlightuserdata(L, lctx->param);
			lua_pushinteger(L, lctx->szParam);
		}
		// exec svc loader
		r = lua_pcall(L, argNum, 0, 0);
		// destroy param
		param_destroy(lctx);
		if (r != LUA_OK) {
			*errMsg = (char*)lua_tostring(L, -1);
			ret = -3;
			break;
		}
		lctx->L = L;
		ret = 0;
	} while (false);
	if (ret != 0) {   // init luaState failed
		if(L) {
			lua_close(L);
		}
		lctx->L = nullptr;
		*errCode = ret;
		return 0;
	}
	*errCode = 0;
	return 1;
}

static inline void param_destroy(struct lpps_svc_ctx* lctx)
{
	if (lctx) {
		if (lctx->src) {
			pps_free(lctx->src);
			lctx->src = nullptr;
		}
		if (lctx->param) {
			pps_free(lctx->param);
			lctx->param = nullptr;
		}
	}
}

static inline void lctx_destroy(struct lpps_svc_ctx* lctx)
{
	if (lctx) {
		if (lctx->L) {
			lua_close(lctx->L);
			lctx->L = nullptr;
		}
		param_destroy(lctx);
		lctx->svc = nullptr;
		delete lctx;
	}
}

static void* lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct lpps_svc_ctx* ctx = (struct lpps_svc_ctx*)ud;
	//printf("lua mem: %lu\n", ctx->mem);
	if(nsize == 0)
	{
		if (ptr)
		{
			pps_free(ptr);
			ctx->mem -= osize;
		}
		return NULL;
	}
	else
	{	
		void* ptrNew = pps_realloc(ptr, nsize);
		if (ptrNew)
		{
			if (ptr)
			{
				ctx->mem += nsize - osize;
			}
			else
			{	
				ctx->mem += nsize;
			}
		}
		return ptrNew;
	}	
}


