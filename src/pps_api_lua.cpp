#include "pps_api_lua.h"

#include <cstring>
#include "pps_macro.h"
#include "pps_service.h"
#include "pps_config.h"
#include "pipes.h"
#include "pps_adapter.h"
#include "pps_message.h"
#include "pps_malloc.h"
#include "lpps_adapter.h"
#include "lpps_lua_seri.h"
#include "pps_worker.h"
#include "pps_timer.h"
#include "pps_sysapi.h"
#include "pps_logger.h"
#include "lpps_sharetable.h"

#include <thread>
#include <chrono>

static void* lua_seri_realloc(
	void* ptr,
	size_t szOldData,
	size_t szNewPrefer,
	size_t szNewMin,
	size_t* szNewActual);

static int reg_cb(lua_State* L, int idx, lua_Integer* cb)
{
	luaL_checktype(L, idx, LUA_TFUNCTION);
	int type = lua_rawgeti(L, LUA_REGISTRYINDEX, *cb);
	lua_pop(L, 1); 
	int setSucc = 0;
	if (type != LUA_TFUNCTION)  //  cb has not set
		{
			*cb = luaL_ref(L, LUA_REGISTRYINDEX);
			setSucc = 1;
		}
	return setSucc;
}

static int l_dispatch(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	int type = luaL_checkinteger(L, 1);
	int setSucc = 0;
	if (type == 1) { // luaCb
		setSucc = reg_cb(L, 2, &lctx->luaCb);
	}
	else if (type == 2) { // timeCb
		setSucc = reg_cb(L, 2, &lctx->timerCb);
	}
	else if (type == 3) { // netCb
		setSucc = reg_cb(L, 2, &lctx->netCb);
	}
	else {
		return luaL_error(L, "unknown dispatch type: %d", type);
	}
	lua_pushboolean(L, setSucc);
	return 1;
}

static void* pack_param(lua_State*L, struct pps_service*svc, int start, int paramNum, 
	uint32_t mType, uint32_t* szParam) 
{
	void* bufParam = nullptr;
	if (paramNum > 0) { // has param
	    struct lpps_lua_seri_ctx seriCtx;
		seriCtx.buf = svc->curWorker->tmpBuf.buf;
		seriCtx.bufCap = svc->curWorker->tmpBuf.cap;
		seriCtx.fnRealloc = lua_seri_realloc;
		seriCtx.bytesPacked = 0;
		//
		void* bufPacked = lua_seri_pack(L, start, &seriCtx);
		if (bufPacked) {
			size_t szPacked = seriCtx.bytesPacked;
			bufParam = pps_malloc(szPacked);
			memcpy(bufParam, bufPacked, szPacked);
			*szParam = msg_size_combine(mType, szPacked);
			//
			svc->curWorker->tmpBuf.buf = (char*)bufPacked;
			svc->curWorker->tmpBuf.cap = seriCtx.bufCap;
			return bufParam;
		}
	}
	*szParam = msg_size_combine(mType, 0);
	return bufParam;
}
static int l_newservice(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	const char* src = luaL_checkstring(L, 1);
	int type = luaL_checkinteger(L, 2);
	if(type != 1 && type != 2){
		return luaL_error(L, "newservice error: type invalid: %d", type);
	}
	struct pps_service* svc = lctx->svc;
	int argNum = lua_gettop(L);  // src, paramVars ...
	//
	uint32_t szParam = 0;
	void* bufParam = pack_param(L, svc, 3, lua_gettop(L) - 2, LPPS_MTYPE_LUA, &szParam);
	//
	int64_t id = lpps_newservice(src, bufParam, szParam, svc->pipes, svc, type==2);
	if (id >= 0) { // create svc succ
		lua_pushnumber(L, id);
	}
	else {
		lua_pushboolean(L, false);
	}
	return 1;
}

static int l_unpackmsg(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	void* ptrParam = lua_touserdata(L, 1);
	uint32_t szParam = luaL_checkinteger(L, 2);
	uint32_t type = msg_type(szParam);
	//
	int argNum = lua_seri_unpack(L, ptrParam, msg_size_real(szParam), nullptr, nullptr);
	return argNum;
}

static int l_send(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pps_service* svc = lctx->svc;
	// to, session, type, ...
	int64_t to = luaL_checknumber(L, 1);
	int32_t session = luaL_checknumber(L, 2);
	uint32_t type = luaL_checkinteger(L, 3);
	//
	uint32_t cntTo;
	uint32_t idxTo = luasvcid_decode(to, &cntTo);
	struct pps_message msg;
	uint32_t szParam = 0;
	//
	/*
	if(type >= LPPS_MTYPE_LUABEGIN) { // lua service event
		pack_param(L, svc, 4, lua_gettop(L) - 3, &bufParam, &szParam);
	}else {   // inner event
		szParam = msg_size_combine(type, 0);
	}*/
	void* bufParam = pack_param(L, svc, 4, lua_gettop(L) - 3, type, &szParam);
	msg.from_cnt = svc->svcCntLocal;
	msg.to_cnt = cntTo;
	msg.idx_pair = idxpair_encode(svc->svcIdx, idxTo);
	msg.session = session;
	msg.data = bufParam;
	msg.size = szParam;
	//
	if(svc_sendmsg(idxTo, &msg, svc->pipes, svc) > 0) {   // send succ
		lua_pushboolean(L, true);
	}else {   // send failed
		lua_pushboolean(L, false);
		if (bufParam) {
			lpps_destroy_msg(&msg, lctx);
		}
	}
	return 1;
}

static int l_timeout(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	int32_t delay = luaL_checkinteger(L, 1);
	int32_t session = luaL_checkinteger(L, 2);
	int32_t repeat = luaL_checkinteger(L, 3);
	if (delay < 0 || session < 1 || repeat < 0) {
		luaL_error(L, "invalid timeout param: delay=%d, ss=%d, repeat=%d", 
			delay, session, repeat);
	}
	struct pps_service* s = lctx->svc;
	svc_check_mqtimer_init(s);
	struct timer_msg m;
	m.svcIdx = s->svcIdx;
	m.svcCnt = s->svcCntLocal;
	m.delay = delay;
	m.session = session;
	m.repeat = repeat;
	m.expiration = timer_clock_now_ms(s->pipes) + delay;
	
	timer_add_task(&m, s->timer);
	return 0;
}
static int l_clock(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	lua_pushnumber(L, timer_clock_now_ms(lctx->svc->pipes));
	return 1;
}
static int l_now(lua_State* L)	
{
	lua_pushnumber(L, sysapi_system_now());
	return 1;
}

static int l_selfmsg(lua_State* L)	
{
	int msgType = luaL_checkinteger(L, 1);
	int session = luaL_checkinteger(L, 2);
	int cmd = luaL_checkinteger(L, 3);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pps_service* svc = lctx->svc;
	struct pps_message msg;
	msg.from_cnt = cmd;
	msg.to_cnt = svc->svcCntLocal;
	msg.idx_pair = idxpair_encode(0, svc->svcIdx);
	msg.session = -session;
	msg.data = nullptr;
	msg.size = msg_size_combine(MTYPE_SVC_SELFMSG, 0);
	if(svc_sendmsg(svc->svcIdx, &msg, svc->pipes, svc) > 0) {   // send succ
		lua_pushboolean(L, true);
	}else {   // send failed
		lua_pushboolean(L, false);
	}
	return 1;
}
static int l_exit(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	lctx->exit = true;
	if (svc_exit(lctx->svc)) {
		lua_pushboolean(L, true);
	}
	else {
		lua_pushboolean(L, false);
	}
	return 1;
}
static int l_free(lua_State* L)	
{
	void* ptr = lua_touserdata(L, 1);
	if (ptr) {
		pps_free(ptr);
	}
	return 0;
}

static int l_id(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	//struct pps_service* s = lctx->svc;
	//lua_pushnumber(L, luasvcid_encode(s->svcIdx, s->svcCntLocal));
	lua_pushnumber(L, lctx->svcId);
	return 1;
}
static int trans_args_to_str(lua_State* L, uint32_t* pSize, char** pData, struct pps_worker* worker)
{
	int argNum = lua_gettop(L);
	if (argNum < 2) {
		return 0;
	}
	// calc size
	uint32_t size = 0;
	size_t sz;
	if(argNum > worker->arrPtrCharSize){
		if(worker->arrPtrChar){
			delete[] worker->arrPtrChar;
		}
		worker->arrPtrChar = new char*[argNum];
		worker->arrPtrCharSize = argNum;
	}
	if(argNum > worker->arrIntSize){
		if(worker->arrInt){
			delete[] worker->arrInt;
		}
		worker->arrInt = new int[argNum];
		worker->arrIntSize = argNum;
	}
	char** arrArgStr = worker->arrPtrChar;
	int* arrArgSize = worker->arrInt;
	//const char* arrArgStr[argNum];
	//int arrArgSize[argNum];

	int validArgNum = 0;
	for(int i=2; i<=argNum; ++i) {
		const char* str = lua_tolstring(L, i, &sz);
		if (str) {   // can convert to string
			size += sz;
			arrArgSize[validArgNum] = sz;
			arrArgStr[validArgNum] = (char*)str;
			++validArgNum;
		}
		else {
			int type = lua_type(L, i);
			if (type == LUA_TBOOLEAN) {
				if (lua_toboolean(L, i)) {
					sz = 4;
					arrArgStr[validArgNum] = "true";
				}
				else {
					sz = 5;
					arrArgStr[validArgNum] = "false";
				}
				size += sz;
				arrArgSize[validArgNum] = sz;
				++validArgNum;
			}
			else {
				break;
			}
		}
	}
	if (size <= 0) {
		return 0;
	}
	// fill data
	*pSize = size;
	if(validArgNum == 1) {
		*pData = (char*)arrArgStr[0];
		return 1;
	}else {
		char* ptr = (char*)pps_malloc(size);
		int cursor = 0;
		for (int i = 0; i < validArgNum; ++i) {
			memcpy(ptr + cursor, arrArgStr[i], arrArgSize[i]);
			cursor += arrArgSize[i];
		}
		*pData = ptr;
		return validArgNum;
	}
}
static int l_log(lua_State* L)	
{
	int level = luaL_checkinteger(L, 1);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pps_service* s = lctx->svc;
	uint32_t size = 0;
	char* pData = nullptr;
	int argNum = trans_args_to_str(L, &size, &pData, s->curWorker);
	if (argNum < 1) {
		return luaL_error(L, "args invalid for logger");
	}
	// send
	struct pps_logger* logger = s->pipes->logger;
	struct logger_msg* pMsg = logger_send_begin(size,
		//luasvcid_encode(s->svcIdx, s->svcCntLocal),
		lctx->svcId, level,
		logger);
	logger_send_end(pData, pMsg, logger);
	//
	if(argNum > 1) {
		pps_free(pData);
	}
	return 0;
}
static int l_shutdown(lua_State* L)
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	pps_shutdown(lctx->svc->pipes);
	return 0;
}
static int l_sleep(lua_State* L)	
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	lua_Integer milli = luaL_checkinteger(L, 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(milli));
	return 0;
}

static int l_stb_loadfile(lua_State* L)
{
	const char* file = luaL_checkstring(L, 1);
	const char* mode = lua_tostring(L, 2);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pipes* pipes = lctx->svc->pipes;
	return sharetb_loadfile(L, pipes->shareTableMgr, file, pipes->config->lua_path, mode);
}
static int l_stb_query(lua_State* L)
{
	const char* file = luaL_checkstring(L, 1);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pipes* pipes = lctx->svc->pipes;
	return sharetb_query(L, pipes->shareTableMgr, file);
}

int luapps_api_openlib(lua_State* L)
{
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "id", l_id },
		//
		{ "now", l_now },
		{ "clock", l_clock },
		{ "timeout", l_timeout },
		{ "unpackMsg", l_unpackmsg },
		{ "newservice", l_newservice },
		{ "send", l_send },
		{ "dispatch", l_dispatch },
		{ "selfmsg", l_selfmsg},
		{ "exit", l_exit },
		{ "free", l_free },
		{ "log", l_log },
		{"shutdown", l_shutdown},
		//
		{"stb_loadfile", l_stb_loadfile},
		{"stb_query", l_stb_query},
		// debug
		//{"sleep", l_sleep},
		//
		{ NULL, NULL },
	};
	// 
	lua_createtable(L, 0, sizeof(l) / sizeof(l[0]) - 2);
	// 
	lua_getfield(L, LUA_REGISTRYINDEX, LPPS_SVC_CTX);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx *)lua_touserdata(L, -1);
	if (lctx == NULL) {
		return luaL_error(L, "lua-service has not initialized");
	}
	luaL_setfuncs(L, l, 1);
	
	return 1;
}

static void* lua_seri_realloc(
	void* ptr,
	size_t szOldData,
	size_t szNewPrefer,
	size_t szNewMin,
	size_t* szNewActual)
{
	void* ptrNew = pps_realloc(ptr, szNewPrefer);	
	*szNewActual = szNewPrefer;
	return ptrNew;
}

