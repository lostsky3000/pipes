
-- const
local CLIENT_PLUGIN_AUTH = 1<<19


local sock = require('pipes.socket')
local _readLen = sock.readlen
local _strUnpack = string.unpack
local _strByte = string.byte
local _strSub = string.sub

local function _getByte2(data,i)
	return _strUnpack('<I2',data,i)
end
local function _getByte3(data,i)
	return _strUnpack('<I3',data,i)
end
local function _getByte4(data,i)
	return _strUnpack('<I4',data,i)
end
local function _getCStr(data,i)
	return _strUnpack('z',data,i)
end


local function _readPack(self)
	local id = self._id
	local data = _readLen(id,4)  -- read pack head: payloadLen(3byte) + seqId(1byte)
	if not data then  -- conn gone
		self._alive = nil
		return false,'conn has gone'
	end
	local len = _getByte3(data,1)
	if len == 0 then 
		return false,'pack len is 0'
	end
	--print('packLen: ',len)
	if len > self._maxPackSize then
		return false,'packet size is too big, '..len..'>'..self._maxPackSize
	end
	local packSeqId = _strByte(data,4)
	--print('packSeqId: ',packSeqId)
	data = _readLen(id,len)   -- read payload
	if not data then
		return false,'conn has gone'
	end
	local tp = _strByte(data,1)
	return data,len,tp
end

local function _parseHandshakeV10(self,data,len)
	local svrVer,pos = _getCStr(data,2)
	print('svrVer: ',svrVer)
	self._svrVer = svrVer
	local thId,pos = _getByte4(data,pos)
	print('threadId: ',thId)
	local scramble1 = _strSub(data, pos, pos + 8 - 1)
    if not scramble1 then
    	return false, "1st part of scramble not found"
    end
    --print('scramble1: ',scramble1)
    pos = pos + 9 -- skip filler
    local capability1,pos = _getByte2(data,pos)
    self._svrLang = _strByte(data,pos)
    pos = pos + 1
    print('svrLang: ',self._svrLang)
    self._svrStatus,pos = _getByte2(data,pos)
    print('svrStatus: ',self._svrStatus)
    local capability2,pos = _getByte2(data,pos)
    local capability = capability1|capability2<<16
    self._svrCapability = capability
    local cliPlugAuth = (capability & CLIENT_PLUGIN_AUTH) ~= 0
    print('cliPlugAuth: ',cliPlugAuth)
    local authPlugLen
    if cliPlugAuth then
    	authPlugLen = _strByte(data,pos)
    else
    	authPlugLen = 0
    end
    pos = pos + 11
    print('authPlugLen: ',authPlugLen)
    

end

local function _doLogin(self)
	-- read handshake
	local data,len,tp = _readPack(self)
	if not data then  -- error
		return false,len
	end
	--print('handShake ver: ',tp)
	if tp == 10 then  -- handshake v10
		_parseHandshakeV10(self,data,len)
	else -- unknown handshake version
		return false,'unknown handshake ver: '..tp
	end
end

local m = {}
function m.new()
	local o = setmetatable({}, {__index=m})
	return o
end
function m:alive()
	return self._alive
end
function m:connect(arg)
	local host = arg.host
	local port = arg.port
	local user = arg.user
	local pwd = arg.pwd
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
	_doLogin(self)
	return true
end


return m



