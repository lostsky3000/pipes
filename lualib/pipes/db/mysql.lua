
-- const
local CLI_PLUGIN_AUTH = 1<<19
local CLI_PROTOCOL_41 = 512
local CLI_SSL = 2048

local sock = require('pipes.socket')
local _c = LPPS_C_LIB
--
local _send = sock.send
local _readLen = sock.readlen
local _read = sock.read
local _strUnpack = string.unpack
local _strPack = string.pack
local _strByte = string.byte
local _strChar = string.char
local _strSub = string.sub
local _strGsub = string.gsub
local _strRep = string.rep
local _strFormat = string.format
local _max = math.max
local _sha1 = _c.crpt_sha1
local _tbInsert = table.insert

local function _getInt2(data,i)
	return _strUnpack('<I2',data,i)
end
local function _getInt3(data,i)
	return _strUnpack('<I3',data,i)
end
local function _getInt4(data,i)
	return _strUnpack('<I4',data,i)
end
local function _getInt8(data,i)
	return _strUnpack('<I8',data,i)
end
local function _getCStr(data,i)
	return _strUnpack('z',data,i)
end
local function _setInt3(i)
	return _strPack('<I3',i)
end
local function _getLenEncInt(data,i)
	local lead = _strByte(data,i)
	if lead < 251 then -- cur 1byte
		return lead,i+1
	elseif lead == 0xfc then -- next 2bytes
		return _getInt2(data,i+1), i+3
	elseif lead == 0xfd then -- next 3bytes
		return _getInt3(data,i+1), i+4
	elseif lead == 0xfe then -- next 8bytes
		return _getInt8(data,i+1), i+9
	else --
		error('invalid lenEncLead: '..lead)
	end
end
local function _getLenEncStr(data,i)
	local len,i = _getLenEncInt(data,i)
	return _strSub(data,i,i+len-1),i+len
end

local function _parseErrPack(data,len)
	local errCode = _getInt2(data,2)
	local sqlState = _strSub(data,5,9)
	local errMsg = _strSub(data,10,len)
	return errCode,errMsg,sqlState
end
local function _parseEofPack(data)
	local warningNum = _getInt2(data,2)
	local svrStatus = _getInt2(data,4)
	return warningNum,svrStatus
end
local function _parseOkPack(data)
	local pos = 2
	local affectedRows,pos = _getLenEncInt(data,pos)
	local lastInsertId,pos = _getLenEncInt(data,pos)
	local svrStatus = _getInt2(data,pos)
	pos = pos + 2
	local warningNum = _getInt2(data,pos)
	pos = pos + 2
	return affectedRows,lastInsertId,warningNum,svrStatus
end

local function _readPack(self)
	local id = self._id
	local data = _readLen(id,4)  -- read pack head: payloadLen(3byte) + seqId(1byte)
	if not data then  -- conn gone
		self._alive = nil
		return false,'conn has gone'
	end
	local len = _getInt3(data,1)
	if len == 0 then 
		return false,'pack len is 0'
	end
	--print('packLen: ',len)
	if len > self._maxPackSize then
		return false,'packet size is too big, '..len..'>'..self._maxPackSize
	end
	local packSeqId = _strByte(data,4)
	self._packSeqId = packSeqId
	--print('packSeqId: ',packSeqId)
	data = _readLen(id,len)   -- read payload
	if not data then
		self._alive = nil
		return false,'conn has gone'
	end
	local tp
	local code = _strByte(data,1)
	if code == 0x00 then  -- ok
		tp = 'OK'
	elseif code <= 250 then
		tp = 'DATA'
	elseif code == 0xff then -- error
		tp = 'ERR'
	elseif code == 0xfe then  -- eof
		tp = 'EOF'
	end
	return data,len,tp,code
end

local function _wrapPack(self,payload,len)
	self._packSeqId = self._packSeqId + 1
	local packet = _setInt3(len) .. _strChar(self._packSeqId) .. payload
    return packet
end
local function _parseHandshakeV10(self,data,len)
	local svrVer,pos,_ = _getCStr(data,2)
	--print('svrVer: ',svrVer)
	self._svrVer = svrVer
	local thId,pos = _getInt4(data,pos)
	--print('threadId: ',thId)
	local scramble1 = _strSub(data, pos, pos + 8 - 1)
    if not scramble1 then
    	return false, "1st part of scramble not found"
    end
    --print('scramble1: ',scramble1)
    pos = pos + 9 
    local capability,pos = _getInt2(data,pos)
    self._svrLang = _strByte(data,pos)
    pos = pos + 1
    --print('svrLang: ',self._svrLang)
    _,pos = _getInt2(data,pos)
    --print('svrStatus: ',self._svrStatus)
    local capability2,pos = _getInt2(data,pos)
    capability = capability|capability2<<16
    if capability&CLI_PROTOCOL_41 == 0 then  -- svr too old
    	return false,'server is too old'
    end
    --[[  debug check
    if capability& (1<<21) ~= 0 then --CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA 
    	print('============ CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA')
    end
    if capability&(1<<20) ~= 0 then --CLIENT_CONNECT_ATTRS
    	print('============ CLIENT_CONNECT_ATTRS')
    end
    ]]
    self._svrCapability = capability
    local cliPlugAuth = (capability & CLI_PLUGIN_AUTH) ~= 0
    --print('cliPlugAuth: ',cliPlugAuth)
    local authPlugLen
    if cliPlugAuth then
    	authPlugLen = _strByte(data,pos)
    else
    	authPlugLen = 0
    end
    pos = pos + 11
    --print('authPlugLen: ',authPlugLen)
    authPlugLen = _max(13,authPlugLen-8)
    local scramble2 = _strSub(data,pos,pos+authPlugLen-1-1) --? why -2 not -1
    --print('scramble2: ',scramble2)
    self._scramble = scramble1..scramble2
    local authPlugName
    if cliPlugAuth then
    	pos = pos + authPlugLen
    	authPlugName = _strSub(data,pos,len-1)
    	self._authPlugName = authPlugName
    end
    if not authPlugName or authPlugName ~= 'mysql_native_password' then
    	return false,'unsupport auth plugin: '..(authPlugName or 'null')
    end
    print('authPlugName: ',authPlugName)
    return true
end
local function _calcAuthToken(pwd, scramble)
    if pwd == "" then
        return ""
    end
    local s1 = _sha1(pwd)
    local s2 = _sha1(s1)
    local s3 = _sha1(scramble .. s2)
	local i = 0
	return _strGsub(s3,".",
		function(x)
			i = i + 1
			-- ~ is xor in lua 5.3
			return _strChar(_strByte(x) ~ _strByte(s1, i))
		end)
end
local function _genSqlErrStr(data,len)
	local errCode,errMsg,sqlState = _parseErrPack(data,len)
	return _strFormat('errno:%d, err:%s, sqlState:%s',errCode,errMsg,sqlState)
end
local function _readAuthRsp(self)
	local data,len,tp = _readPack(self)
	if not data then
		return false,len
	end
	if tp == 'ERR' then
		return false,_genSqlErrStr(data,len)
	end
	--print('readAuthRsp: ',tp,len,_strByte(data,1))
	if tp ~= 'OK' then
		return false,'error authRspPackType: '.._strByte(data,1)
	end
	return true
end
local function _doLogin(self,user,pwd,db)
	-- read handshake pack
	local data,len,tp = _readPack(self)
	if not data then  -- error
		return false,len
	end
	--print('handShake ver: ',tp)
	local ok,err
	if tp == 'ERR' then
		error( _genSqlErrStr(data,len) )
	end
	ok,err = _parseHandshakeV10(self,data,len)
	if not ok then  -- parse handshake failed
		return ok,err
	end
	-- handshake response
	local cliFlags = 260047
	self._cliFlags = cliFlags
	--local capability = self._svrCapability
	local authToken = _calcAuthToken(pwd,self._scramble)
	local req = _strPack('<I4I4c24zs1zzB',
		cliFlags,
		self._maxPackSize,
		_strRep('\0',24),
		user,
		authToken,
		db,
		self._authPlugName,
		0)
	local payloadLen = #req
	--print('reqPayload: ',req,payloadLen)
	local pack = _wrapPack(self,req,payloadLen)
	ok = _send(self._id,pack)
	if not ok then -- conn gone
		self._alive = nil
		return false,'conn has gone'
	end
	-- read auth rsp
	return _readAuthRsp(self)
end

local m = {}
function m.new()
	local o = setmetatable({}, {__index=m,__gc=m.close})
	return o
end
function m:alive()
	return self._alive
end
function m:connect(arg)
	local host = arg.host or ''
	local port = arg.port or 0
	local user = arg.user or ''
	local pwd = arg.pwd or ''
	local db = arg.db or ''
	local tmout = arg.timeout
	if not tmout then
		tmout = 10000
	end
	local maxPackSize = arg.max_pack_size
	if not maxPackSize then
		maxPackSize = 1024 * 1024
	end
	self._maxPackSize = maxPackSize
	--
	local id,err = sock.connect(host, port, {timeout=tmout}) 
	if not id then
		return false,err
	end
	self._alive = true
	self._id = id
	local ok,err = _doLogin(self,user,pwd,db)
	if not ok and self._alive then  -- error but connAlive, close
		self._alive = nil
		sock.close(id)
	end
	print('mysql server version: ',self._svrVer)
	return ok,err
end

local function _readResultSet(self)
	local data,len,tp = _readPack(self)
	if not data then
		return 'ERR',len
	end
	if tp == 'ERR' then
		return 'ERR',_genSqlErrStr(data,len)
	end
	if tp == 'OK' then
		return 'OK'
	end
	--print('_readResultSet: ',tp,len)
	-- type is data(text resultset)
	local capab = self._cliFlags --self._svrCapability
	if capab&(1<<25) ~= 0 then --CLIENT_OPTIONAL_RESULTSET_METADATA 
		--print('CLIENT_OPTIONAL_RESULTSET_METADATA ')
		error('CLIENT_OPTIONAL_RESULTSET_METADATA not supported')
	end
	local cols = {}
	local rows = {}
	local colNum = _getLenEncInt(data,1)
	print('colNum: ',colNum)
	-- read colDef packs
	local _,pos
	local cnt = 0
	while(true)
	do
		data,len,tp = _readPack(self)
		if not data then
			return 'ERR',len
		end
		if tp == 'ERR' then
			return 'ERR',_genSqlErrStr(data,len)
		end
		--print('colDefPack: ',tp,len)
		local col = {}
		pos = 1
		_,pos = _getLenEncStr(data,pos) -- catalog
		_,pos = _getLenEncStr(data,pos) -- schema
		_,pos = _getLenEncStr(data,pos) -- vtTable
		_,pos = _getLenEncStr(data,pos) -- phyTable
		col.vt,pos = _getLenEncStr(data,pos) --vtCol
		_,pos = _getLenEncStr(data,pos) -- phyCol
		_,pos = _getLenEncInt(data,pos)
		_,pos = _getInt2(data,pos) -- charset
		_,pos = _getInt4(data,pos) -- colLen
		col.type = _strByte(data,pos) --colType
		pos = pos + 1
		col.flags,pos = _getInt2(data,pos)
		_ = _strByte(data,pos)
		pos = pos + 1
		--print('colVtName: ',col.vt)
		--[[
		for k,v in pairs(colDef) do
			print('colItem: ',k,v)
		end]]
		cnt = cnt + 1
		cols[cnt] = col
		if cnt >= colNum then
			break
		end
	end
	local cliEofDeprecate = capab&(1<<24) ~= 0
	if not cliEofDeprecate then  -- not CLIENT_DEPRECATE_EOF (1UL << 24
		--read a eofpack as end of colDefs
		data,len,tp = _readPack(self)
		if not data then
			return 'ERR',len
		end
	end
	-- read rows data until terminate pack
	local ret = 'DONE'
	while(true)
	do
		data,len,tp = _readPack(self)
		if not data then
			return 'ERR',len
		end
		if tp == 'DATA' then
			local val
			pos = 1
			local row = {}
			for i=1,colNum do
				val = _strByte(data,pos)
				if val == 0xfb then
					pos = pos + 1
					--row[i] = nil
					row[cols[i].vt] = nil
				else
					val,pos = _getLenEncStr(data,pos)
					--row[i] = val
					row[cols[i].vt] = val
				end
			end
			_tbInsert(rows,row)
		else
			if tp == 'EOF' then
				local warnNum,svrStatus = _parseEofPack(data)
				--print('rows endwith eofPack,',warnNum,svrStatus)
				if svrStatus&8 ~= 0 then --SERVER_MORE_RESULTS_EXISTS 
					--print('has multiResult')
					ret = 'AGAIN'
				end
				break
			end
			if tp == 'OK' then
				local afRows,lastIstId,warnNum,svrStatus = _parseOkPack(data)
				--print('rows endwith okPack')
				if svrStatus&8 ~= 0 then --SERVER_MORE_RESULTS_EXISTS 
					--print('has multiResult')
					ret = 'AGAIN'
				end
				break
			end
			if tp == 'ERR' then
				--print('rows endwith errPack')
				return 'ERR',_genSqlErrStr(data,len)
				--break
			end
		end
	end
	return ret,rows
end
local function _readQueryRsp(self)
	local tp,rows = _readResultSet(self)
	if tp == 'OK' then
		return true
	elseif tp == 'DONE' then  -- one rs
		return 1,rows
	elseif tp == 'ERR' then -- err
		return false,rows
	elseif tp == 'AGAIN' then
		local mtrs = {}
		_tbInsert(mtrs, rows)
		local num = 1
		while(tp == 'AGAIN')
		do
			tp,rows = _readResultSet(self)
			if tp == 'ERR' then
				return false,rows
			end
			_tbInsert(mtrs, rows)
			num = num + 1
		end
		return num,mtrs
	end
end
local function _wrapQuery(self,query)
	local payload = _strChar(0x03)..query -- COM_QUERY: 0x03
	self._packSeqId = -1
	return _wrapPack(self,payload,#payload)
end
function m:query(str)
	local pack = _wrapQuery(self,str)
	--print('query: ',pack)
	local ok,err = _send(self._id,pack)
	if not ok then
		self._alive = nil
		return false,'conn has gone'
	end
	return _readQueryRsp(self)
end
function m:serverVer()
	return self._svrVer
end
function m:close()
	if self._alive then
		self._alive = nil
		sock.close(self._id)
	end
end

return m



