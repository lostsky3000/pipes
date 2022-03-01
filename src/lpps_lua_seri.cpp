#include "lpps_lua_seri.h"

#include <cstring>
#include <cstdint>

extern "C" {
#include "lualib.h"
#include "lauxlib.h"
}

#define  LPPS_SERI_PTR_SUPPORT

#define LTYPE_NIL 0
#define LTYPE_BOOL 1
#define LTYPE_INTEGER 2
#define LTYPE_FLOAT 3
#define LTYPE_STRING 4
#define LTYPE_POINTER 5
#define LTYPE_TABLE 6
          
#define MAX_DEPTH 32

#define ENCODE_TYPE(T,V) (T<<4)|V
struct pack_data
{
	void* buf;
	size_t buf_cap;
	size_t idx_write;
	int has_error;
	fn_lua_seri_realloc fn_realloc;
};

static void buf_set(struct pack_data* pack, int idxStart, void* data, size_t size)
{
	memcpy((unsigned char*)pack->buf + idxStart, data, size);
}

static void buf_write(struct pack_data* pack, void*data, size_t size)
{
	size_t capLeft = pack->buf_cap - pack->idx_write;
	if (size > capLeft)  // need expand
		{
			size_t szPrefer = pack->buf_cap * 2;
			size_t szMin = pack->idx_write + size;
			if (szPrefer < szMin)
			{
				szPrefer = szMin;
			}
			size_t szActual = 0;
			void* newBuf = pack->fn_realloc(pack->buf, pack->idx_write, szPrefer, szMin, &szActual);
			//
			pack->buf = newBuf;
			pack->buf_cap = szActual;
		}
	memcpy((unsigned char*)pack->buf + pack->idx_write, data, size);
	pack->idx_write += size;
}
static void pack_free(struct pack_data* pack)
{
	if (pack->buf)
	{
		//pack->fn_free(pack->buf, pack->udata);
		pack->buf = NULL;
		pack->buf_cap = 0;
	}	
}
static void pack_init(
	struct pack_data* pack,
	lua_State* L,
	int paramNum,
	void* buf,
	size_t bufCap,
	fn_lua_seri_realloc fnRealloc)
{
	pack->buf = buf;
	pack->buf_cap = bufCap;
	pack->fn_realloc = fnRealloc;
	pack->idx_write = 0;
	pack->has_error = 0;
	// write param num
	uint8_t num = paramNum;
	buf_write(pack, &num, sizeof(num));
}
static void pack_one(struct pack_data* pack, lua_State* L, int idx, int depth);
static void write_nil(struct pack_data* pack)
{
	uint8_t val = ENCODE_TYPE(LTYPE_NIL, 0);
	buf_write(pack, &val, sizeof(val));
}
static void write_bool(struct pack_data* pack, int b)
{
	uint8_t val = ENCODE_TYPE(LTYPE_BOOL, b);
	buf_write(pack, &val, sizeof(val));
}
static void write_integer(struct pack_data* pack, lua_Integer val)
{
	int len = 0;
	lua_Integer abs = val >= 0 ? val : -val;
	if ((abs = abs >> 7) == 0)   // 1byte
		{
			len = 1;
		}
	else if ((abs = abs >> 8) == 0)   // 2byte
		{
			len = 2;
		}
	else if ((abs = abs >> 16) == 0)  // 4 byte
		{
			len = 4;
		}
	else   // 8 byte
		{
			len = 8;
		}
	uint8_t type = ENCODE_TYPE(LTYPE_INTEGER, len);
	buf_write(pack, &type, sizeof(type));  // write type
	buf_write(pack, &val, len);    // write data
}
static void write_float(struct pack_data* pack, lua_Number val)
{
	size_t sz = sizeof(val);
	uint8_t type = ENCODE_TYPE(LTYPE_FLOAT, sz);
	buf_write(pack, &type, sizeof(type));     // write type
	buf_write(pack, &val, sz);      // write data
}
static void write_string(struct pack_data* pack, const char* str, size_t sz)
{
	int lenBytes;
	if (sz <= 0xff)
	{
		lenBytes = 1;
	}
	else if (sz <= 0xffff)
	{
		lenBytes = 2;
	}
	else
	{
		lenBytes = 4;
	}
	uint8_t type = ENCODE_TYPE(LTYPE_STRING, lenBytes);
	buf_write(pack, &type, sizeof(type));   // write type
	buf_write(pack, &sz, lenBytes);   // write string-length
	buf_write(pack, (void*)str, sz);   // write string
}
static void write_pointer(struct pack_data* pack, lua_State* L, int idx)
{
	void * ptr = lua_touserdata(L, idx);
	size_t sz = sizeof(ptr);
	uint8_t type = ENCODE_TYPE(LTYPE_POINTER, sz);
	buf_write(pack, &type, sizeof(type));
	buf_write(pack, &ptr, sz);
	/*
	if (pack->fn_ptr_iter != NULL)  // ptr need cb
		{
			pack->fn_ptr_iter(ptr, pack->udata);
		}
		*/
}

static uint32_t write_table_array(struct pack_data* pack, lua_State*L, int idx, int depth, int* idxHashLenBuf)
{
	size_t len = lua_rawlen(L, idx);
	// write table head
	int lenBytes = 0;
	if (len > 0)
	{
		if (len <= 0xff)
		{
			lenBytes = 1;
		}
		else if (len <= 0xffff)
		{
			lenBytes = 2;
		}
		else
		{
			lenBytes = 4;
		}
	}
	uint8_t type = ENCODE_TYPE(LTYPE_TABLE, lenBytes);
	buf_write(pack, &type, sizeof(type));
	if (len > 0)  // write array-item length
		{
			buf_write(pack, &len, lenBytes);
		}
	// use 4 bytes to store hash-len
	*idxHashLenBuf = pack->idx_write;
	buf_write(pack, &len, 4);    // write placeholder
	// pack arr-items
	int i;
	for (i = 1; i <= len; ++i)
	{
		lua_rawgeti(L, idx, i);
		pack_one(pack, L, -1, depth);
		lua_pop(L, 1);
	}
	return len;
}
static uint32_t write_table_hash(struct pack_data* pack, lua_State*L, int idx, int depth, uint32_t arrLen)
{
	uint32_t hashLen = 0;
	if (idx < 0)
	{
		idx = lua_gettop(L) + idx + 1;
	}
	lua_pushnil(L);
	while (lua_next(L, idx) != 0)
	{
		if (lua_isinteger(L, -2))
		{
			lua_Integer i = lua_tointeger(L, -2);
			if (i > 0 && i <= arrLen)   // has proc by write_table_array
				{
					lua_pop(L, 1);
					continue;
				}
		}
		pack_one(pack, L, -2, depth);   // pack key
		pack_one(pack, L, -1, depth);   // pack value
		lua_pop(L, 1);
		++hashLen;
	}
	return hashLen;
}
static void write_table(struct pack_data* pack, lua_State* L, int idx, int depth)
{
	luaL_checkstack(L, LUA_MINSTACK, NULL);
	int idxHashLenBuf = 0;
	uint32_t arrLen = write_table_array(pack, L, idx, depth, &idxHashLenBuf);
	uint32_t hashLen = write_table_hash(pack, L, idx, depth, arrLen);
	buf_set(pack, idxHashLenBuf, &hashLen, 4);   //write hash-len
	// write table-end 
	write_nil(pack);
}

static void pack_one(struct pack_data* pack, lua_State* L, int idx, int depth)
{
	if (depth > MAX_DEPTH) {
		pack_free(pack);
		luaL_error(L, "can't pack too depth table: %d", depth);
	}
	int type = lua_type(L, idx);
	switch (type)
	{
	case LUA_TNUMBER: {
			if (lua_isinteger(L, idx))   // integer
				{
					write_integer(pack, lua_tointeger(L, idx));
				}
			else  // float
				{
					write_float(pack, lua_tonumber(L, idx));
				}	
			break;
		}
	case LUA_TSTRING: {
			size_t sz;
			const char* str = lua_tolstring(L, idx, &sz);
			write_string(pack, str, sz);
			break;
		}
	case LUA_TTABLE: {
			write_table(pack, L, idx, depth + 1);
			break;
		}
#ifdef LPPS_SERI_PTR_SUPPORT
	case LUA_TLIGHTUSERDATA: {
			write_pointer(pack, L, idx);	
			break;
		}
#endif // LPPS_SERI_PTR_SUPPORT
	
	case LUA_TBOOLEAN: {
			write_bool(pack, lua_toboolean(L, idx));
			break;
		}
	case LUA_TNIL: {
			write_nil(pack);
			break;
		}
	default: {
			pack_free(pack);
			luaL_error(L, "pack error, unsupport type: %s", lua_typename(L, type));
			break;
		}
	}
}

void* lua_seri_pack(
	lua_State* L,
	int from,
	struct lpps_lua_seri_ctx* seriCtx)
{
	int top = lua_gettop(L);
	if (from > top)
	{
		seriCtx->bytesPacked = 0;
		luaL_error(L, "seri-pack from index invalid: from=%d, top=%d", from, top);
		//*szOut = 0;
		return NULL;
	}
	int paramNum = top - from + 1;
	if (paramNum > 255)
	{
		luaL_error(L, "seri-pack too many params, can not more than 255: from=%d, top=%d", from, top);
	}
	//
	struct pack_data pack;
	pack_init(&pack, L, paramNum, seriCtx->buf, seriCtx->bufCap, seriCtx->fnRealloc);
	//
	int i = from;
	for (; i <= top; ++i)
	{
		pack_one(&pack, L, i, 0);
	}
	//
	void* bufOut = pack.buf;
	seriCtx->bytesPacked = pack.idx_write;
	seriCtx->bufCap = pack.buf_cap;
	return bufOut;
}


// unpack
struct unpack_data
{
	int idx_read;
	//
	int param_num;
	size_t buf_size;
	unsigned char* buf;
	void* ud;
	fn_lua_deseri_ptr_iter it_ptr;
};

static inline void
invalid_stream_line(lua_State *L, struct unpack_data *unpack, int line) {
	int len = unpack->idx_read;
	luaL_error(L, "Invalid serialize stream %d (line:%d)", len, line);
}

#define invalid_stream(L,UP) invalid_stream_line(L,UP,__LINE__)

static uint8_t read_ubyte(struct unpack_data* unpack)
{
	return unpack->buf[unpack->idx_read++];
}
static uint8_t* read_type(struct unpack_data* unpack)
{
	if (unpack->idx_read >= unpack->buf_size)
	{
		return NULL;
	}
	return &unpack->buf[unpack->idx_read++];
}
static lua_Integer read_luainteger(struct unpack_data* unpack, int len)
{
	lua_Integer ret;
	if (len == 1)
	{
		ret = (char)unpack->buf[unpack->idx_read++];
	}
	else if (len == 2)
	{
		ret = *(int16_t*)(unpack->buf + unpack->idx_read);
		unpack->idx_read += 2;
	}
	else if (len == 4)
	{
		ret = *(int32_t*)(unpack->buf + unpack->idx_read);
		unpack->idx_read += 4;
	}
	else  // 8
		{
			ret = *(int64_t*)(unpack->buf + unpack->idx_read);
			unpack->idx_read += 8;
		}
	return ret;
}
static lua_Number read_luanumber(struct unpack_data* unpack)
{
	lua_Number num = *(lua_Number*)(unpack->buf + unpack->idx_read);
	unpack->idx_read += sizeof(lua_Number);
	return num;
}
static char* read_luastring(struct unpack_data* unpack, int lenBytes, size_t* sz)
{
	if (lenBytes == 1)  // length use 1 byte
		{
			*sz = *(uint8_t*)(unpack->buf + unpack->idx_read);
			++unpack->idx_read;
		}
	else if (lenBytes == 2)  // length use 2 byte
		{
			*sz = *(uint16_t*)(unpack->buf + unpack->idx_read);
			unpack->idx_read += 2;
		}
	else
	{
		*sz = *(uint32_t*)(unpack->buf + unpack->idx_read);
		unpack->idx_read += 4;
	}
	char* ptr = (char*)(unpack->buf + unpack->idx_read);
	unpack->idx_read += *sz;
	return ptr;
}
static void* read_luauserdata(struct unpack_data* unpack)
{
	void** ptr = (void**)(unpack->buf + unpack->idx_read);
	unpack->idx_read += sizeof(void*);
	return *ptr;
}
static void push_value(struct unpack_data* unpack, lua_State* L, int type, int typeVal);
static void unpack_one(struct unpack_data* unpack, lua_State* L)
{
	uint8_t* pType = read_type(unpack);
	if (pType)  
	{
		uint8_t type = *pType;
		push_value(unpack, L, type >> 4, type & 0xf);
		return;
	}
	//error
	invalid_stream(L, unpack);
}
static void unpack_table(struct unpack_data* unpack, lua_State* L, int lenBytes)
{	
	size_t arrLen = 0;
	if (lenBytes == 1)  // 1byte for length
		{
			arrLen = unpack->buf[unpack->idx_read++];
		}
	else if (lenBytes == 2) // 2byte for length
		{
			arrLen = *(uint16_t*)(unpack->buf + unpack->idx_read);
			unpack->idx_read += 2;
		}
	else if (lenBytes == 4) // 4byte for length
		{
			arrLen = *(uint32_t*)(unpack->buf + unpack->idx_read);
			unpack->idx_read += 4;
		}
	// read hash-len
	size_t hashLen = *(uint32_t*)(unpack->buf + unpack->idx_read);
	unpack->idx_read += 4;
	// create table
	luaL_checkstack(L, LUA_MINSTACK, NULL);
	lua_createtable(L, arrLen, hashLen);
	if (arrLen > 0)  // has arr-item
		{
			int i;
			for (i = 1; i <= arrLen; ++i)
			{
				unpack_one(unpack, L);
				lua_rawseti(L, -2, i);
			}
		}
	// unpack hash-item
	for(;  ;) {
		unpack_one(unpack, L);   // key
		if(lua_isnil(L, -1)) {
			 // table end
			lua_pop(L, 1);
			return;
		}
		unpack_one(unpack, L);   //value
		lua_rawset(L, -3);
	}
}
static void unpack_init(struct unpack_data* unpack, void*buf, size_t szBuf, 
	fn_lua_deseri_ptr_iter itPtr, void* ud)
{   
	unpack->buf = (unsigned char*)buf;
	unpack->idx_read = 0;
	if (buf)
	{
		unpack->buf_size = szBuf;
		unpack->param_num = read_ubyte(unpack);
	}
	else
	{
		unpack->buf_size = 0;
		unpack->param_num = 0;
	}
	unpack->it_ptr = itPtr;
	unpack->ud = ud;
}
static void push_value(struct unpack_data* unpack, lua_State* L, int type, int typeVal)
{
	switch (type)
	{
	case LTYPE_BOOL: {
			lua_pushboolean(L, typeVal);
			break;
		}
	case LTYPE_INTEGER: {
			lua_Integer num = read_luainteger(unpack, typeVal);
			lua_pushinteger(L, num);
			break;
		}
	case LTYPE_FLOAT: {
			lua_Number num = read_luanumber(unpack);
			lua_pushnumber(L, num);
			break;
		}
	case LTYPE_STRING: {
			size_t sz = 0;
			char* str = read_luastring(unpack, typeVal, &sz);
			lua_pushlstring(L, str, sz);
			break;
		}
#ifdef LPPS_SERI_PTR_SUPPORT
	case LTYPE_POINTER: {
			void* ptr = read_luauserdata(unpack);
			lua_pushlightuserdata(L, ptr);
			if (unpack->it_ptr && ptr)
			{
				unpack->it_ptr(ptr, unpack->ud);
			}
			break;
		}	  
#endif // LPPS_SERI_PTR_SUPPORT

	case LTYPE_TABLE: {
			unpack_table(unpack, L, typeVal);
			break;
		}
	case LTYPE_NIL: {
			lua_pushnil(L);
			break;
		}
	default:
		invalid_stream(L, unpack);
		break;
	}
}
int lua_seri_unpack(lua_State*L, void*buf, size_t szBuf, 
		fn_lua_deseri_ptr_iter itPtr, void* ud)
{
	struct unpack_data unpack;
	unpack_init(&unpack, buf, szBuf, itPtr, ud);
	for (;;)
	{
		uint8_t* pType = read_type(&unpack);
		if (pType == NULL)  // no more data
			{
				break;
			}
		uint8_t type = *pType;
		push_value(&unpack, L, type >> 4, type & 0xf);
	}
	
	return unpack.param_num;
}
