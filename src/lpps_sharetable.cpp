#include "lpps_sharetable.h"
#include "pps_api_lua_3rd.h"
#include "lpps_adapter.h"
#include "pps_api_lua.h"

#define METATABLE_NAME "LpPs_ShArE_mEtA"

struct table_ctx
{
	int len;
	int itAllDone;
	int itCnt;
	struct share_table* rootCtx;
	void* tbOri;
	lua_State* Lit;
};

static inline int metafn_gc(lua_State* Lproxy)
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
static int create_proxy_table(lua_State* Lproxy, void* tbOri, struct share_table* rootCtx)
{	
	// create proxy table
	lua_newtable(Lproxy);  //lua_createtable
	luaL_getmetatable(Lproxy, METATABLE_NAME);
	lua_setmetatable(Lproxy, -2);
	//
	void* ptrTbProxy = (void*)lua_topointer(Lproxy, -1);
	struct table_ctx* ctx = new struct table_ctx;
	ctx->tbOri = tbOri;
	ctx->rootCtx = rootCtx;
	ctx->len = -1;
	ctx->itAllDone = 0;
	ctx->Lit = nullptr;
	ctx->itCnt = 0;
	lua_pushlightuserdata(Lproxy, ptrTbProxy);
	lua_pushlightuserdata(Lproxy, ctx);
	lua_settable(Lproxy, LUA_REGISTRYINDEX);
	printf("newProxyTable: %p, ctx: %p\n", ptrTbProxy, ctx);  // debug
	return 1;
}

static int move_val(lua_State* Lto, int tp, struct share_table* rootCtx, lua_State* Lfrom)
{
	int popLoriNum = 2;  // val & table
	if (tp == LUA_TSTRING) {
		size_t len;
		const char* val = lua_tolstring(Lfrom, -1, &len);
		lua_pushlstring(Lto, val, len);
		lua_settable(Lto, -3);
		lua_pushlstring(Lto, val, len);
	} else if (tp == LUA_TNUMBER) {
		if(lua_isinteger(Lfrom, -1)){
			int num = lua_tointeger(Lfrom, -1);
			lua_pushinteger(Lto, num);
			lua_settable(Lto, -3);
			lua_pushinteger(Lto, num);
		}else{
			lua_Number num = lua_tonumber(Lfrom, -1);
			lua_pushnumber(Lto, num);
			lua_settable(Lto, -3);
			lua_pushnumber(Lto, num);
		}
	} else if (tp == LUA_TTABLE) {
		void* tbOri = (void*)lua_topointer(Lfrom, -1);
		lua_pushlightuserdata(Lfrom, tbOri);
		lua_insert(Lfrom, -2);
		lua_settable(Lfrom, LUA_REGISTRYINDEX);  // keep tbOri alive
		popLoriNum = 1;
		//
		create_proxy_table(Lto, tbOri, rootCtx);    // now, new tbProxy on the top
		lua_pushvalue(Lto, -1); // copy tbProxy to top
		lua_insert(Lto, -4);    // mv top tbProxy to -4
		lua_settable(Lto, -3);  // set tbProxy to tbParent
		lua_pop(Lto, 1);        // pop tbParent, left tbProxy on top
	} else if (tp == LUA_TBOOLEAN) {
		int b = lua_toboolean(Lfrom, -1);
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
		lua_pushnil(Lto);
		lua_settable(Lto, -3);
		lua_pushnil(Lto);
	}
	if(popLoriNum > 0){
		lua_pop(Lfrom, popLoriNum);
	}
	return 1;
}
static int metafn_len(lua_State* L)
{
	void* tbCur = (void*)lua_topointer(L, 1);
	// get ctx
	lua_pushlightuserdata(L, tbCur);
	lua_gettable(L, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	//
	if(ctx->len > -1){  // has got len
		lua_pushinteger(L, ctx->len);
		return 1;
	}
	//
	int len = 0;
	void* tbOri = ctx->tbOri;
	struct share_table* rootCtx = ctx->rootCtx;
	lua_State* Lori = rootCtx->L;
	{
		std::lock_guard<std::mutex> lock(rootCtx->mtx);
		lua_pushlightuserdata(Lori, tbOri);
		lua_gettable(Lori, LUA_REGISTRYINDEX);  // make tbOri to the top
		lua_len(Lori, -1);
		len = lua_tointeger(Lori, -1);
		lua_pop(Lori, 2);  // pop len & table
	}
	ctx->len = len;
	lua_pushinteger(L, len);
	return 1;
}

static int luaB_next(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_settop(L, 2);  /* create a 2nd argument if there isn't one */
	if (lua_next(L, 1))
		return 2;
	else {
		lua_pushnil(L);
		return 1;
	}
}
static int metafn_pairs_it(lua_State* L)
{
	void* tbCur = (void*)lua_topointer(L, 1);
	// get ctx
	lua_pushlightuserdata(L, tbCur);
	lua_gettable(L, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	//
	struct share_table* rootCtx = ctx->rootCtx;
	lua_State* Lori = rootCtx->L;
	int& itCnt = ctx->itCnt;
	lua_State* Lit = ctx->Lit;
	std::lock_guard<std::mutex> lock(rootCtx->mtx);
	if(Lit == nullptr){  // no L for it, create
		lua_pushlightuserdata(Lori, tbCur);
		Lit = lua_newthread(Lori);
		lua_settable(Lori, LUA_REGISTRYINDEX);  //keep Lit alive
		ctx->Lit = Lit;
	}else{
		if(itCnt == 0){   // 1st it, clear Lit
			lua_pop(Lit, lua_gettop(Lit));  
		}
	}
	if(itCnt++ == 0){  // it start, init
		lua_pushlightuserdata(Lit, ctx->tbOri);  // get table
		lua_pushnil(Lit);
	}
	if(lua_next(Lit, -2) != 0){  // has val
		
	}else{ // it done
		ctx->itAllDone = 1;
		// rm Lit?
		return 0;
	}
	return 2;
}
static int metafn_pairs(lua_State* L)
{
	void* tbCur = (void*)lua_topointer(L, 1);
	// get ctx
	lua_pushlightuserdata(L, tbCur);
	lua_gettable(L, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	//
	if(ctx->itAllDone){  // has set all val
		lua_pushcfunction(L, luaB_next);  /* will return generator, */
		lua_pushvalue(L, 1);  /* state, */
		lua_pushnil(L);  /* and initial value */
		return 3;
	}
	//
	ctx->itCnt = 0;   // reset itCnt
	lua_pushcfunction(L, metafn_pairs_it);
	lua_pushvalue(L, 1);
	return 2;
}
static int metafn_index(lua_State* L)
{
	void* tbCur = (void*)lua_topointer(L, 1);
	// get ctx
	lua_pushlightuserdata(L, tbCur);
	lua_gettable(L, LUA_REGISTRYINDEX);
	struct table_ctx* ctx = (struct table_ctx*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	//
	void* tbOri = ctx->tbOri;
	struct share_table* rootCtx = ctx->rootCtx;
	lua_State* Lori = rootCtx->L;
	int tp = lua_type(L, 2);
	if(tp == LUA_TSTRING){
		const char* key = lua_tostring(L, 2);
		printf("meta_index_table(str), tb=%p, key=%s\n", tbCur, key); // debug
		// get val from tableOri
		std::lock_guard<std::mutex> lock(rootCtx->mtx);
		lua_pushlightuserdata(Lori, tbOri);
		lua_gettable(Lori, LUA_REGISTRYINDEX);  // make tbOri to the top
		lua_pushstring(Lori, key);
		tp = lua_gettable(Lori, -2);   // get value
		return move_val(L, tp, rootCtx, Lori);
	}else if(tp == LUA_TNUMBER){
		int key = lua_tonumber(L, 2);
		printf("meta_index_table(num), tb=%p, key=%d\n", tbCur, key); // debug
		// get val from tableOri
		std::lock_guard<std::mutex> lock(rootCtx->mtx);
		lua_pushlightuserdata(Lori, tbOri);
		lua_gettable(Lori, LUA_REGISTRYINDEX);  // make tbOri to the top
		lua_pushinteger(Lori, key);
		tp = lua_gettable(Lori, -2);   // get value
		return move_val(L, tp, rootCtx, Lori);
	}
	// treat as nil
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
	void* table = (void*)lua_topointer(L, -1);
	lua_pushlightuserdata(L, table);
	lua_insert(L, -2);
	lua_settable(L, LUA_REGISTRYINDEX);   // keep rootTable alive
	//
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
		lua_pushcfunction(L, metafn_index);
		lua_settable(L, -3);
		// set gc cb
		lua_pushstring(L, "__gc");
		lua_pushcfunction(L, metafn_gc);
		lua_settable(L, -3);
		// set len cb
		lua_pushstring(L, "__len");
		lua_pushcfunction(L, metafn_len);
		lua_settable(L, -3);
		// set paris cb
		lua_pushstring(L, "__pairs");
		lua_pushcfunction(L, metafn_pairs);
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



