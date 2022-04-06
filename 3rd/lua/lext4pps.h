#ifndef LEXT4PPS_H
#define LEXT4PPS_H

#include "llimits.h"
#include "lstate.h"
#include "lobject.h"
#include "ltable.h"

LUA_API const TValue* (lext_getval_bystr)(lua_State *L, Table* tb, const char* key);

LUA_API const TValue* (lext_getval_bynum)(Table* tb, int key);

LUA_API int (lext_gettblen)(Table* tb);
//
LUA_API int (lext_tonumber)(const TValue* val, lua_Number* num);

/*
LUA_API int (lext_tt_isstring)(const TValue*val);
LUA_API int (lext_tt_isnumber)(const TValue*val);
LUA_API int (lext_tt_istable)(const TValue*val);
LUA_API int (lext_tt_isboolen)(const TValue*val);
*/

#endif // !LEXT4PPS_H
