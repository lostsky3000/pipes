#define LUA_CORE   // for enable dllexport
#include "lext4pps.h"
#include "lstring.h"
#include "lvm.h"

LUA_API const TValue* (lext_getval_bystr)(lua_State *L, Table* tb, const char* key)
{
	TString* keyLocal = luaS_new(L, key);
	return luaH_getstr(tb, keyLocal);
}

LUA_API const TValue* (lext_getval_bynum)(Table* tb, int key)
{
	return luaH_getint(tb, key);
}

LUA_API int (lext_gettblen)(Table* tb)
{
	return luaH_getn(tb);
}


LUA_API int (lext_tonumber)(const TValue* val, lua_Number* num)
{
	return tonumber(val, num);
}

/*
LUA_API int (lext_tt_isstring)(const TValue*val)
{
	return ttisstring(val);
}
LUA_API int (lext_tt_isnumber)(const TValue*val)
{
	return ttisnumber(val);
}
LUA_API int (lext_tt_istable)(const TValue*val)
{
	return ttistable(val);
}
LUA_API int (lext_tt_isboolen)(const TValue*val)
{
	return ttisboolean(val);
}
*/

