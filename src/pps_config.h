#ifndef PPS_CONFIG_H
#define PPS_CONFIG_H

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#include <cstdint>

struct pps_config
{
	uint32_t worker_num;
	uint32_t timer_num;
	uint32_t net_num;
	char* lua_path;
	char* lua_cpath;
	char* boot_service;
};

struct pps_config* config_init(lua_State* L);

void config_deinit(struct pps_config* config);

void config_set(lua_State* L, struct pps_config* config);


#endif // !PPS_CONFIG_H

