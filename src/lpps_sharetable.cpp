#include "lpps_sharetable.h"


#define ROOT_TABLE_NAME "RTTABLE"
#define METATABLE_NAME "LpPs_SHARE_META"

struct table_ctx
{
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
static int create_proxy_table(lua_State* Lproxy, Table* tb, struct share_table* rootCtx)
{	
	// create proxy table
	lua_newtable(Lproxy);  //lua_createtable
	luaL_getmetatable(Lproxy, METATABLE_NAME);
	lua_setmetatable(Lproxy, -2);
	//
	void* ptrTbProxy = (void*)lua_topointer(Lproxy, -1);
	struct table_ctx* ctx = new struct table_ctx;
	ctx->table = tb;
	ctx->root = rootCtx;
	lua_pushlightuserdata(Lproxy, ptrTbProxy);
	lua_pushlightuserdata(Lproxy, ctx);
	lua_settable(Lproxy, LUA_REGISTRYINDEX);
	
	printf("newProxyTable: %p, ctx: %p\n", ptrTbProxy, ctx);  // debug
	return 1;
}

static int move_val_to_proxy(lua_State* Lproxy, const TValue* val, lua_State* Lori)
{
	/*
	if(ttisstring(val)){
		char* str = svalue(val);
		lua_pushstring(Lproxy, str);
		lua_settable(Lproxy, -3);
		lua_pushstring(Lproxy, str);
	}else if(ttisnumber(val)){
	
	}else if(ttisboolean(val)){
	
	}else if(ttisnil(val)){
		lua_pushnil(Lproxy);
		lua_settable(Lproxy, -3);
		lua_pushnil(Lproxy);
	}else if(ttistable(val)){
	
	}else{
		return 0;
	}
	*/
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
	if(tp == LUA_TSTRING){
		const char* key = lua_tostring(L, 2);
		// get val from tableOri
		/*
		std::lock_guard<std::mutex> lock(ctx->root->mtx);
		TString* keyOri = luaS_new(rootCtx->L, key);
		const TValue* val = luaH_getstr(tbOri, keyOri);
		// set val to curL
		move_val_to_proxy(L, val, rootCtx->L);
		*/
		lext_copy_bystr(rootCtx->L, key, L);
	}else if(tp == LUA_TNUMBER){
	
	}

	return 1;
}
static inline int share_load_file(struct share_table* tb, const char* file, const char* luaPath)
{
	std::lock_guard<std::mutex> lock(tb->mtx);
	lua_State* L = tb->L;
	lua_pushstring(L, luaPath);
	lua_setglobal(L, "LUA_PATH");
	lua_pushstring(L, file);
	lua_setglobal(L, "TABLE_NAME");
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
		lua_pushstring(L, "load sharetable failed");
		return 0;
	}
	Table* table = (Table*)lua_topointer(L, -1);
	lua_setglobal(L, ROOT_TABLE_NAME);  // keep rootTable alive
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

//
int sharetb_loadfile(lua_State* L, struct share_table_mgr* mgr, const char* file, const char* luaPath)
{
	struct share_table* tb = get_share_table(mgr, file);
	if(tb == nullptr){   // not loaded yet
		tb = new_share_table();
		if(!share_load_file(tb, file, luaPath)){  // load failed
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
	}
	lua_pop(L, 1); // pop metatable from curStack
	//
	create_proxy_table(L, tb->tbRoot, tb);
	return 1;
}


//
int sharetb_mgr_init(struct share_table_mgr* mgr)
{

	return 1;
}
void sharetb_mgr_deinit(struct share_table_mgr* mgr)
{

}



