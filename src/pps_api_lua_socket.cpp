#include "pps_api_lua_socket.h"
#include "lpps_adapter.h"
#include "pps_net.h"
#include "pipes.h"
#include "pps_service.h"
#include "pps_worker.h"
#include "tcp_protocol.h"
#include <cstring>

static int l_test(lua_State* L)
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pipes* pipes = lctx->svc->pipes;
	//
	SOCK_ADDR addr;
	int ret = sock_addr_pton(&addr, "127.0.0.1");
	ret &= sock_addr_pton(&addr, "0.0.0.0");
	
	struct tcp_server_cfg cfg;
	int addrNum = 0;
	//ret &= sock_addr_pton(&cfg.addrs[idx++], "0.0.0.0");
	ret &= sock_addr_pton(&cfg.addrs[addrNum++], "192.168.0.24");
	ret &= sock_addr_pton(&cfg.addrs[addrNum++], "127.0.0.1");
	cfg.addrNum = addrNum;
	
	int n = sock_addr_isinany(&cfg.addrs[0]);
	n = sock_addr_isinany(&cfg.addrs[1]);
	n = sock_addr_isinany(&cfg.addrs[2]);
	
	if (!ret) {
		return luaL_error(L, "invalid addrs");
	}
	cfg.port = 10086;  //11300;
	cfg.backlog = 128;
	cfg.recvBuf = 1024;
	cfg.sendBuf = 2048;
	//
	struct netreq_src src;
	src.idx = 1;
	src.cnt = 2;
	
	ret = net_tcp_listen(&src, pipes, &cfg);
	if (ret == 0) {  // listen succ
		lua_pushstring(L, "succ");
		return 1;
	}
	else {   // listen failed
		lua_pushstring(L, "failed, ");
		lua_pushinteger(L, ret);
		return 2;
	}
}

static int l_init(lua_State* L)
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	svc_check_mqnet_init(lctx->svc);      // ensure mqnet not null
	return 0;
}

static int parse_protocol(lua_State* L, int tbIdx, struct protocol_cfg** pCfgOut)
{
	int protocolType = PPSTCP_PROTOCOL_INVALID;
	// check protocol type first
	lua_pushnil(L);
	while(lua_next(L, tbIdx) != 0){
		int keyType = lua_type(L, -2);
		int valType = lua_type(L, -1);
		if(keyType == LUA_TSTRING){
			const char* key = lua_tostring(L, -2);
			if(strcmp(key, "type") == 0){   // parse type field
				if(valType != LUA_TSTRING){
					return luaL_error(L, "protocol type invalid: %d", valType);
				}
				const char* val = lua_tostring(L, -1);
				if(!protocol_check_cfgtype(val, &protocolType)){
					return luaL_error(L, "protocol unsupport: %s", val);
				}
			}
		}
		lua_pop(L, 1);
	}
	if(protocolType == PPSTCP_PROTOCOL_INVALID){
		return luaL_error(L, "protocol type not specify");
	}
	// alloc cfg
	struct protocol_cfg* pCfg = protocol_cfg_alloc(protocolType);
	// check cfg detail
	char errBuf[128];
	lua_pushnil(L);
	while (lua_next(L, tbIdx) != 0) {
		int keyType = lua_type(L, -2);
		int valType = lua_type(L, -1);
		if (keyType == LUA_TSTRING) {
			const char* key = lua_tostring(L, -2);
			int valType = lua_type(L, -1);
			int ret = -1;
			if(valType == LUA_TSTRING){
				ret = protocol_cfg_add_item_str(pCfg, key, lua_tostring(L, -1), errBuf, 128);
			}else if(valType == LUA_TNUMBER){
				ret = protocol_cfg_add_item_int(pCfg, key, (int)lua_tonumber(L, -1), errBuf, 128);
			}else{
				return luaL_error(L, "protocol cfg valType unsupport: key=%s, valType=%s", key, lua_typename(L, valType));
			}
			if(ret == -1){
				protocol_cfg_free(pCfg);
				return luaL_error(L, "%s", errBuf);
			}
		}
		lua_pop(L, 1);
	}
	// check if cfg is all done
	if(!protocol_cfg_whole_check(pCfg, errBuf, 128)){
		protocol_cfg_free(pCfg);
		return luaL_error(L, "%s", errBuf);
	}
	*pCfgOut = pCfg;
	return 0;
}
static int l_listen(lua_State* L)
{
	// session, port, backlog, sendBuf, recvBuf, addrs
	int argNum = lua_gettop(L);
	if (argNum < 6) {
		return luaL_error(L, "listen() args num invalid: %d", argNum);
	}
	int session = luaL_checkinteger(L, 1);
	if (session <= 0) {
		return luaL_error(L, "listen() session invalid: %d", session);
	}
	int port = luaL_checkinteger(L, 2);
	if (port <= 0) {
		return luaL_error(L, "listen() port invalid: %d", port);
	}
	int backlog = lua_tointeger(L, 3);
	if (backlog < 0) {
		return luaL_error(L, "listen() backlog invalid: %d", backlog);
	}
	backlog = backlog == 0 ? SOCK_BACKLOG_DEF : backlog;
	//
	int sendBuf = lua_tointeger(L, 4);
	if (sendBuf < 0) {
		return luaL_error(L, "listen() sendBufLen invalid: %d", sendBuf);
	}
	sendBuf = sendBuf == 0 ? SOCK_SEND_BUF_MIN : sendBuf;
	//
	int recvBuf = lua_tointeger(L, 5);
	if (recvBuf < 0) {
		return luaL_error(L, "listen() recvBufLen invalid: %d", recvBuf);
	}
	recvBuf = recvBuf == 0 ? SOCK_RECV_BUF_MIN : recvBuf;
	//
	struct tcp_server_cfg cfgTcp;
	int addrNum = argNum - 6;
	if (addrNum > SOCK_TCP_LISTEN_ADDR_MAX) {
		return luaL_error(L, "listen() addr num over limit: %d>%d", addrNum, SOCK_TCP_LISTEN_ADDR_MAX);
	}
	if (addrNum <= 0) {   //  not specify addr list, use localhost
		sock_addr_pton(&cfgTcp.addrs[0], "127.0.0.1");
		cfgTcp.addrNum = 1;
	}
	else {
		for (int i=0; i<addrNum; ++i) {
			const char * strAddr = luaL_checkstring(L, i + 7);
			if (strAddr) {
				if (!sock_addr_pton(&cfgTcp.addrs[i], strAddr)) {
					return luaL_error(L, "listen() addr invalid: %s", strAddr);
				}
			}
			else {
				return luaL_error(L, "listen() addr invalid");
			}
		}
		cfgTcp.addrNum = addrNum;
	}
	// check protocol
	int type = lua_type(L, 6);
	if (type == LUA_TNIL) {
		cfgTcp.protocolCfg = nullptr;
	} else if (type == LUA_TTABLE) {
		struct protocol_cfg* pCfg = nullptr;
		parse_protocol(L, 6, &pCfg);
		if (pCfg == nullptr) {
			return luaL_error(L, "protocol invalid");
		}
		cfgTcp.protocolCfg = pCfg;
	} else {
		return luaL_error(L, "listen() protocolLuaType invalid: %d", type);
	}
	//
	cfgTcp.port = port;
	cfgTcp.backlog = backlog;
	cfgTcp.sendBuf = sendBuf;
	cfgTcp.recvBuf = recvBuf;
	//
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pps_service* s = lctx->svc;
	struct pipes* pipes = s->pipes;
	//
	struct netreq_src src;
	src.idx = s->svcIdx;
	src.cnt = s->svcCntLocal;
	src.session = session;
	//
	int ret = net_tcp_listen(&src, pipes, &cfgTcp);
	if (ret == 0) {   // send listenReq succ
		lua_pushboolean(L, true);
		return 1;
	}
	else {  //  send listenReq failed
		protocol_cfg_free(cfgTcp.protocolCfg);
		lua_pushboolean(L, false);
		lua_pushinteger(L, ret);
		return 2;
	}
}
static int l_connect(lua_State* L)
{
	// session, host, port, timeout, sendBufLen, recvBufLen
	int argNum = lua_gettop(L);
	if (argNum < 3) {
		return luaL_error(L, "connect() args num invalid: %d", argNum);
	}
	int session = luaL_checkinteger(L, 1);
	const char* host = luaL_checkstring(L, 2);
	if (host == nullptr) {
		return luaL_error(L, "connect(), host invalid");
	}
	int port = luaL_checkinteger(L, 3);
	if (port < 1) {
		return luaL_error(L, "connect(), port invalid");
	}
	int timeout = lua_tointeger(L, 4);
	if (timeout < 0) {   
		timeout = 0;
	}
	//
	int sendBuf = lua_tointeger(L, 5);
	if (sendBuf < 0) {
		return luaL_error(L, "connect() sendBufLen invalid: %d", sendBuf);
	}
	sendBuf = sendBuf == 0 ? SOCK_SEND_BUF_MIN : sendBuf;
	//
	int recvBuf = lua_tointeger(L, 6);
	if (recvBuf < 0) {
		return luaL_error(L, "connect() recvBufLen invalid: %d", recvBuf);
	}
	recvBuf = recvBuf == 0 ? SOCK_RECV_BUF_MIN : recvBuf;
	//
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pps_service* s = lctx->svc;
	//
	struct netreq_src src;
	src.idx = s->svcIdx;
	src.cnt = s->svcCntLocal;
	src.session = session;
	struct tcp_connect_cfg cfg;
	cfg.host = (char*)host;
	cfg.port = port;
	cfg.timeout = timeout;
	cfg.sendBuf = sendBuf;
	cfg.recvBuf = recvBuf;
	//
	net_tcp_connect(&src, s->pipes, &cfg);
	
	return 0;
}

static int l_addr(lua_State* L)
{
	int idx = luaL_checkinteger(L, 1);
	int cnt = luaL_checkinteger(L, 2);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pps_service* s = lctx->svc;
	int port;
	char* buf = s->curWorker->tmpBuf.buf;
	if (net_get_remote(s->pipes, idx, cnt, buf, s->curWorker->tmpBuf.cap, &port)) { // succ
		lua_pushstring(L, buf);
		lua_pushinteger(L, port);
		return 2;
	}
	else {  // failed
		lua_pushboolean(L, false);
		return 1;
	}
}

static int l_close(lua_State* L)
{
	int sockIdx = luaL_checkinteger(L, 1);
	int sockCnt = luaL_checkinteger(L, 2);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	net_close_sock(lctx->svc->pipes, sockIdx, sockCnt);
	return 0;
}

static int l_read(lua_State* L)
{
	int32_t sockIdx = luaL_checkinteger(L, 1);
	uint32_t sockCnt = luaL_checkinteger(L, 2);
	int session = luaL_checkinteger(L, 3);
	int decType = luaL_checkinteger(L, 4);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	struct pps_service* s = lctx->svc;
	struct read_arg arg;
	arg.decType = decType;
	arg.isNewRead = true;
	arg.session = session;
	arg.srcIdx = s->svcIdx;
	arg.srcCnt = s->svcCntLocal;
	struct read_tmp tmp;
	tmp.L = L;
	tmp.tmpBuf = &s->curWorker->tmpBuf;
	arg.ud = &tmp;
	arg.cb = lpps_read_msg;
	//
	int ret,trunc;
	if (decType == DECTYPE_LEN) {
		int readLen = luaL_checkinteger(L, 5);
		if (readLen < 1) {
			return luaL_error(L, "readlen arg invalid: %d", readLen);
		}
		arg.maxRead = readLen;
		struct dec_arg_len a;
		a.readLen = readLen;
		arg.decArg = &a;
		ret = net_tcp_read(s->pipes, sockIdx, sockCnt, &arg, &trunc);
	}
	else if (decType == DECTYPE_SEP) {
		size_t slen = 0;
		const char* sep = luaL_checklstring(L, 5, &slen);
		if (slen < 1) {
			return luaL_error(L, "readsep arg invalid: %d", slen);
		}
		int readMax = luaL_checkinteger(L, 6);
		if(readMax < 1){   // invalid
			return luaL_error(L, "readline, packmax invalid: %d", readMax);
		}
		arg.maxRead = readMax;
		struct dec_arg_sep a;
		a.sep = (char*)sep;
		a.sepLen = slen;
		arg.decArg = &a;
		ret = net_tcp_read(s->pipes, sockIdx, sockCnt, &arg, &trunc);
	}
	else {
		int readMax = luaL_checkinteger(L, 5);
		if(readMax < 1){   // invalid
			return luaL_error(L, "read, packmax invalid: %d", readMax);
		}
		arg.maxRead = readMax;
		ret = net_tcp_read(s->pipes, sockIdx, sockCnt, &arg, &trunc);
	} 
	//
	if (ret > 0) {   // read sth
		lua_pushinteger(L, tmp.total);  // read size
		if(ret == 1) {   //  normal read
			lua_pushboolean(L, true);  // isLastRead
			lua_pushboolean(L, trunc);
			return 4;
		}else {   // last read
			lua_pushboolean(L, false);  // isLastRead
			return 3;
		}
	}
	else if (ret == 0) {  // no data now
		lua_pushboolean(L, false);
		lua_pushinteger(L, 0);  // means need yield
		return 2;
	}
	else {  // read status invalid
		lua_pushboolean(L, false);
		lua_pushinteger(L, ret);
		return 2;
	}
}
static int l_send(lua_State* L)
{
	int32_t sockIdx = luaL_checkinteger(L, 1);
	uint32_t sockCnt = luaL_checkinteger(L, 2);
	size_t sz = 0;
	const char * data = luaL_checklstring(L, 3, &sz);
	int isNum = 0;
	int szReq = lua_tointegerx(L, 4, &isNum);
	if (isNum) {
		if (szReq > sz) {
			return luaL_error(L, "socket.send reqSize(%d) > realSize(%d)", szReq, sz);
		}
		sz = szReq;
	}
	// do send
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	int ret = net_tcp_send(lctx->svc->pipes, sockIdx, sockCnt, data, sz);
	lua_pushinteger(L, ret);
	return 1;
}

static int l_hasnet(lua_State* L)
{
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx*)lua_touserdata(L, lua_upvalueindex(1));
	if (pps_hasnet(lctx->svc->pipes)) {
		lua_pushboolean(L, true);
	}
	else {
		lua_pushboolean(L, false);
	}
	return 1;
}

int luapps_api_socket_openlib(lua_State* L)
{
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "test", l_test },
		//
		{"listen", l_listen},
		{"connect", l_connect},
		{"addr", l_addr},
		{"close", l_close},
		{"read", l_read},
		{"send", l_send},
		{"hasnet", l_hasnet},
		{"init", l_init},
		//
		{ NULL, NULL },
	};
	// 
	lua_createtable(L, 0, sizeof(l) / sizeof(l[0]) - 2);
	// 
	lua_getfield(L, LUA_REGISTRYINDEX, LPPS_SVC_CTX);
	struct lpps_svc_ctx* lctx = (struct lpps_svc_ctx *)lua_touserdata(L, -1);
	if (lctx == NULL) {
		return luaL_error(L, "lua-service has not initialized");
	}
	luaL_setfuncs(L, l, 1);
	return 1;
}


