#include "lpps_sharetable.h"
#include "pps_api_lua_3rd.h"
#include "lpps_adapter.h"
#include "pps_api_lua.h"

#define METATABLE_NAME "LpPs_ShArE_mEtA"

struct table_ctx
{
	int len;
	int hasSetPairs;
	struct share_table* root;
	Table* table;
};

static int on_proxy_table_gc(lua_State* Lproxy)
{
	void* ptrTbProxy = (void*)lua_topointer(Lproxy, -1);
	lua_pushlightuserdata(Lproxy, (void*)ptrTbProxy);
	lua_gettable(Lproxy, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(Lproxy, -1);
	printf("onProxyTable gc: %p, ctx: %p\n", ptrTbProxy, ctx);  // debug
	lua_pop(Lproxy, 1); // pop ctx
	delete ctx;
	return 0;
}
static int create_proxy_table(lua_State* Lproxy, Table* tbOri, struct share_table* rootCtx)
{	
	// create proxy table
	lua_newtable(Lproxy);  //lua_createtable
	luaL_getmetatable(Lproxy, METATABLE_NAME);
	lua_setmetatable(Lproxy, -2);
	//
	void* ptrTbProxy = (void*)lua_topointer(Lproxy, -1);
	struct table_ctx* ctx = new struct table_ctx;
	ctx->table = tbOri;
	ctx->root = rootCtx;
	ctx->len = -1;
	ctx->hasSetPairs = 0;
	lua_pushlightuserdata(Lproxy, ptrTbProxy);
	lua_pushlightuserdata(Lproxy, ctx);
	lua_settable(Lproxy, LUA_REGISTRYINDEX);
	printf("newProxyTable: %p, ctx: %p\n", ptrTbProxy, ctx);  // debug
	return 1;
}

static int move_val(lua_State* Lto, const TValue* val, struct share_table* rootCtx, lua_State* Lfrom)
{
	if (ttisstring(val)) {
		char* str = svalue(val);
		lua_pushstring(Lto, str);
		lua_settable(Lto, -3);
		lua_pushstring(Lto, str);
	} else if (ttisnumber(val)) {
		lua_Number lNum;
		if(!lext_tonumber(val, &lNum)){
			lNum = 0;
		}
		lua_pushnumber(Lto, lNum);
		lua_settable(Lto, -3);
		lua_pushnumber(Lto, lNum);
	} else if (ttistable(val)) {
		Table* tbOri =  hvalue(val);
		create_proxy_table(Lto, tbOri, rootCtx);    // now, new tbProxy on the top
		lua_pushvalue(Lto, -1); // copy tbProxy to top
		lua_insert(Lto, -4);    // mv top tbProxy to -4
		lua_settable(Lto, -3);  // set tbProxy to tbParent
		lua_pop(Lto, 1);        // pop tbParent, left tbProxy on top
	} else if (ttisboolean(val)) {
		bool b = !l_isfalse(val);
		lua_pushboolean(Lto, b);
		lua_settable(Lto, -3);
		lua_pushboolean(Lto, b);
	}
	/*
	else if (ttisnil(val)) {
		lua_pushnil(Lto);
		lua_settable(Lto, -3);
		lua_pushnil(Lto);
	}*/ 
	else {  // treat as nil
		//lua_pushnil(Lto);
		//lua_settable(Lto, -3);
		lua_pushnil(Lto);
	}
	return 1;
}

static int meta_pairs_it(lua_State* L)
{
	return 1;
}
static int meta_pairs_table(lua_State* L)
{
	Table* tbCur = (Table*)lua_topointer(L, 1);
	// get ctx
	lua_pushlightuserdata(L, tbCur);
	lua_gettable(L, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	//
	if(!ctx->hasSetPairs){  //  cp all items 
		Table* tbOri = ctx->table;
		struct share_table* rootCtx = ctx->root;
		lua_State* Lori = rootCtx->L;
		{
			std::lock_guard<std::mutex> lock(rootCtx->mtx);
			
		}
		ctx->hasSetPairs = 1;
	}
	// return func,tb,nil
	lua_pushcfunction(L, meta_pairs_it);
	return 1;
}
static int meta_len_table(lua_State* L)
{
	Table* tbCur = (Table*)lua_topointer(L, 1);
	// get ctx
	lua_pushlightuserdata(L, tbCur);
	lua_gettable(L, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	//
	if(ctx->len > -1){  // has got
		lua_pushinteger(L, ctx->len);
		return 1;
	}
	//
	int len = 0;
	Table* tbOri = ctx->table;
	struct share_table* rootCtx = ctx->root;
	{
		std::lock_guard<std::mutex> lock(rootCtx->mtx);
		len = lext_gettblen(tbOri);
	}
	ctx->len = len;
	lua_pushinteger(L, len);
	return 1;
}
static int meta_index_table(lua_State* L)
{
	Table* tbCur = (Table*)lua_topointer(L, 1);
	// get ctx
	lua_pushlightuserdata(L, tbCur);
	lua_gettable(L, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	//
	Table* tbOri = ctx->table;
	struct share_table* rootCtx = ctx->root;
	int tp = lua_type(L, 2);
	const TValue* val;
	if(tp == LUA_TSTRING){
		const char* key = lua_tostring(L, 2);
		printf("meta_index_table(str), tb=%p, key=%s\n", tbCur, key); // debug
		// get val from tableOri
		std::lock_guard<std::mutex> lock(rootCtx->mtx);
		val = lext_getval_bystr(rootCtx->L, tbOri, key);
		return move_val(L, val, rootCtx, rootCtx->L);
	}else if(tp == LUA_TNUMBER){
		int key = lua_tonumber(L, 2);
		printf("meta_index_table(num), tb=%p, key=%d\n", tbCur, key); // debug
		// get val from tableOri
		std::lock_guard<std::mutex> lock(rootCtx->mtx);
		val = lext_getval_bynum(tbOri, key);
		return move_val(L, val, rootCtx, rootCtx->L);
	}
	// treat as nil
	lua_pushnil(L);
	lua_settable(L, -3);
	lua_pushnil(L);
	return 1;
}
static int share_load_file(struct share_table* tb, const char* file, 
	const char* luaPath, const char* mode)
{
	std::lock_guard<std::mutex> lock(tb->mtx);
	lua_State* L = tb->L;
	lua_pushstring(L, luaPath);
	lua_setglobal(L, "LUA_PATH");
	lua_pushstring(L, file);
	lua_setglobal(L, "TABLE_NAME");
	if(mode){
		lua_pushstring(L, mode);
		lua_setglobal(L, "MODE");
		// set 3rdApiLib from json parse
		lua_pushcfunction(L, luapps_api_3rd_openlib);
		lua_setglobal(L, LPPS_OPEN_C_3RD_LIB);
	}
	int ret = luaL_loadfile(L, "./lualib/pps_share_loader.lua");
	if(ret != LUA_OK){
		return 0;
	}
	ret = lua_pcall(L, 0, 1, 0);
	if (ret != LUA_OK) {
		return 0;
	}
	int type = lua_type(L, -1);
	if(type != LUA_TTABLE){
		lua_pushstring(L, "load sharetable failed, type invalid");
		return 0;
	}
	Table* table = (Table*)lua_topointer(L, -1);
	//lua_setglobal(L, ROOT_TABLE_NAME);  // keep rootTable alive
	tb->tbRoot = table;
	return 1;
}
static inline struct share_table* new_share_table()
{
	struct share_table* tb = new struct share_table;
	std::lock_guard<std::mutex> lock(tb->mtx);
	tb->tbRoot = nullptr;
	tb->L = luaL_newstate();
	luaL_openlibs(tb->L);
	return tb;
}
static inline void destroy_share_table(struct share_table* tb)
{
	{
		std::lock_guard<std::mutex> lock(tb->mtx);
		lua_close(tb->L);
		tb->L = nullptr;
		tb->tbRoot = nullptr;
	}
	delete tb;
}
static inline struct share_table* get_share_table(struct share_table_mgr* mgr, const char* name)
{
	std::lock_guard<std::mutex> lock(mgr->mtx);
	SHARE_MAP::iterator it = mgr->map.find(name);
	if(it != mgr->map.end()){  // exist
		return (*it).second;
	}
	return nullptr;
}
static inline struct share_table* add_share_table(struct share_table_mgr* mgr, const char* name, struct share_table* tb)
{
	std::lock_guard<std::mutex> lock(mgr->mtx);
	SHARE_MAP::iterator it = mgr->map.find(name);
	if (it != mgr->map.end()) {  // exist, not add
		return (*it).second;
	}
	mgr->map.insert(std::make_pair(name, tb));
	return tb;
}

static int proxy_query_root(lua_State* L, struct share_table* tb)
{
	// check cur L has metatable
	int ret = luaL_newmetatable(L, METATABLE_NAME);
	if (ret == 0) {   // has set
		if (lua_type(L, -1) != LUA_TTABLE) {  // metaname set by other val
			return luaL_error(L, "sharetable exception: %s", METATABLE_NAME);
		}
	} else {  // metatable 1st set, init
		lua_pushstring(L, "__index");
		lua_pushcfunction(L, meta_index_table);
		lua_settable(L, -3);
		// set gc cb
		lua_pushstring(L, "__gc");
		lua_pushcfunction(L, on_proxy_table_gc);
		lua_settable(L, -3);
		// set len cb
		lua_pushstring(L, "__len");
		lua_pushcfunction(L, meta_len_table);
		lua_settable(L, -3);
		// set paris cb
		lua_pushstring(L, "__pairs");
		lua_pushcfunction(L, meta_pairs_table);
		lua_settable(L, -3);
	}
	lua_pop(L, 1); // pop metatable from curStack
	create_proxy_table(L, tb->tbRoot, tb);
	return 1;
}
//
int sharetb_loadfile(lua_State* L, struct share_table_mgr* mgr, const char* file, 
	const char* luaPath, const char* mode)
{
	struct share_table* tb = get_share_table(mgr, file);
	if(tb == nullptr){   // not loaded yet
		tb = new_share_table();
		if(!share_load_file(tb, file, luaPath, mode)){  // load failed
			int retNum = 1;
			lua_pushboolean(L, false);
			if(lua_type(tb->L, -1) == LUA_TSTRING){  // has set err msg
				const char* err = lua_tostring(tb->L, -1);
				lua_pushstring(L, err); // set err msg for cur L
				retNum = 2;
			}
			destroy_share_table(tb);
			return retNum;
		}
		struct share_table* tbExist = add_share_table(mgr, file, tb);
		if(tbExist != tb){  // already exist, must has been added by another thread
			destroy_share_table(tb);
			tb = tbExist;
		}
	}
	proxy_query_root(L, tb);
	return 1;
}
int sharetb_query(lua_State* L, struct share_table_mgr* mgr, const char* file)
{
	struct share_table* tb = get_share_table(mgr, file);
	if(tb != nullptr){
		proxy_query_root(L, tb);
		return 1;
	}
	return 0;
}

//
int sharetb_mgr_init(struct share_table_mgr* mgr)
{

	return 1;
}
void sharetb_mgr_deinit(struct share_table_mgr* mgr)
{

}



