#ifndef PPS_API_LUA_SOCKET_H
#define PPS_API_LUA_SOCKET_H


extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
int luapps_api_socket_openlib(lua_State* L);


#endif // !PPS_API_LUA_SOCKET_H

