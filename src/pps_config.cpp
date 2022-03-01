
#include <cstring>
#include "pps_config.h"
#include "pps_sysinfo.h"
#include "pps_malloc.h"
#include "pps_macro.h"

static const char* _optCStr(lua_State* L, const char* key, bool strict) 
{
	if (lua_getfield(L, -1, key) == LUA_TSTRING) {
		const char* str = lua_tostring(L, -1);
		lua_pop(L, 1);
		return str;
	}
	lua_pop(L, 1);
	if (strict) {
		luaL_error(L, "no field config: %s", key);
	}
	return NULL;
}
static int _optInt(lua_State* L, const char* key, int def) 
{
	if (lua_getfield(L, -1, key) == LUA_TNUMBER) {
		int ret = luaL_checkinteger(L, -1);
		lua_pop(L, 1);
		return ret;
	}
	lua_pop(L, 1);
	return def;
}

static int _load(lua_State* L, struct pps_config* cfg)
{
	int ret = -1;
	do {
		luaL_checktype(L, -1, LUA_TTABLE);
		const char* strLuaPath = _optCStr(L, "lua_path", 1);
		const char* strLuaCPath = _optCStr(L, "lua_cpath", 1);
		const char* strBootService = _optCStr(L, "boot_service", 1);
		const char* strServiceRoot = _optCStr(L, "service_root", 1);
		int coreNum = sysinfo_core_num();
		int workerNum = _optInt(L, "worker_num", coreNum);
		if (workerNum < 1 || workerNum > MAX_WORKER) {
			return luaL_error(L, "invalid worker num: %d, max_worker=%d", workerNum, MAX_WORKER);
		}
		cfg->worker_num = workerNum;
		//
		int timerNum = _optInt(L, "timer_num", 1);
		if (timerNum < 1 || timerNum > MAX_TIMER) {
			return luaL_error(L, "invalid timer num: %d, max_timer=%d", timerNum, MAX_TIMER);
		}
		cfg->timer_num = timerNum;
		//
		int netNum = _optInt(L, "net_num", 1);   // default net num is 1
		if (netNum < 0 || netNum > MAX_NET) {
			return luaL_error(L, "invalid net num: %d, max_net=%d", netNum, MAX_NET);
		}
		cfg->net_num = netNum;
		//
		cfg->lua_path = (char*)pps_malloc(strlen(strLuaPath) + 1);
		strcpy(cfg->lua_path, strLuaPath);
		//
		cfg->lua_cpath = (char*)pps_malloc(strlen(strLuaCPath) + 1);
		strcpy(cfg->lua_cpath, strLuaCPath);
		//
		cfg->boot_service = (char*)pps_malloc(strlen(strBootService) + 1);
		strcpy(cfg->boot_service, strBootService);
		//
		ret = 0;
	} while (false);
	return ret;
}

struct pps_config* config_init(lua_State* L)
{
	struct pps_config* config = NULL;
	do {
		struct pps_config cfgTmp;
		int tmp = _load(L, &cfgTmp);
		if (tmp) {  // load error
			break;
		}
		//
		config = (struct pps_config*)pps_malloc(sizeof(*config));
		*config = cfgTmp;
	} while (false);
	return config;
}
void config_deinit(struct pps_config* config)
{
	if (config->lua_path) {
		pps_free(config->lua_path);
		config->lua_path = NULL;
	}
	if (config->lua_cpath) {
		pps_free(config->lua_cpath);
		config->lua_cpath = NULL;
	}
	pps_free(config);
}


void set_field_str(lua_State* L, int tableIdx, const char* val, const char* key) 
{
	lua_pushstring(L, val);
	lua_setfield(L, tableIdx, key);
}
void set_field_int(lua_State* L, int tableIdx, int val, const char* key)
{
	lua_pushinteger(L, val);
	lua_setfield(L, tableIdx, key);
}
void config_set(lua_State* L, struct pps_config* config)
{
	lua_createtable(L, 0, 3);
	set_field_str(L, -1, config->lua_path, "lua_path");
	set_field_str(L, -1, config->lua_cpath, "lua_cpath");
	set_field_int(L, -1, config->worker_num, "worker");
}

