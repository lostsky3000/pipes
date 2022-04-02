#ifndef LPPS_SHARETABLE_H
#define LPPS_SHARETABLE_H

#include <unordered_map>
#include <mutex>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
//
#include "ltable.h"
#include "lext4pps.h"
}

typedef std::unordered_map<const char*, struct share_table*> SHARE_MAP;

struct share_table
{
	lua_State* L;
	Table* tbRoot;
	std::mutex mtx;
};
struct share_table_mgr
{
	std::mutex mtx;
	SHARE_MAP map;
};

int sharetb_loadfile(lua_State* L, struct share_table_mgr* mgr, const char* file, const char* luaPath);

//
int sharetb_mgr_init(struct share_table_mgr* mgr);
void sharetb_mgr_deinit(struct share_table_mgr* mgr);


#endif // !LPPS_SHARETABLE_H



