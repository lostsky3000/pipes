
local sock = require('pipes.socket')

local _cs = LPPS_C_SOCK_LIB
local _cSend = _cs.rdssend
local _readLine = sock.readline
local _readLen = sock.readlen
--
local _batInit = _cs.rdsbatinit
local _batAdd = _cs.rdsbatadd
local _batSend = _cs.rdsbatsend

local r = {}

local function _sendCmd(id,cmd,...)
	local ret = _cSend(id._i,id._c,cmd,...);
	if ret > 0 then
		return true
	end
	if ret == 0 then
		return false
	end
	--error('invalid send: '..ret)
end

-- 
local function _parseLine(str)
	-- SimpleString, Error, Integer, BulkString
	local ld = string.byte(str)
	if not ld then -- str is empty
		error('redis rsp invalid(1): '..(str or 'nil'))
	end
	if ld == 43 then  --  + simple string 
		return 1, string.sub(str,2)
	elseif ld == 36 then  -- $  bulk string
		return 2, tonumber(string.sub(str,2))
	elseif ld == 58 then  -- :  integer
		return 3, tonumber(string.sub(str,2))
	elseif ld == 42 then  --  * array
		return 4, tonumber(string.sub(str,2))
	elseif ld == 45 then  --  - (minus) error 
		return 5, string.sub(str,2)
	end
	error('redis rsp invalid(2): '..str)
end

local function _parseArr(id,arr)
	local rsp,err
	local sz = arr.size
	for i=1,sz do
		rsp, err = _readLine(id, '\r\n')
		if not rsp then
			return false, err or 'connect has gone(4)'
		end
		local tp,ld = _parseLine(rsp)
		if tp == 2 then  -- bulk string
			if ld > 0 then   
				rsp, err = _readLen(id, ld+2)  -- include \r\n
				if not rsp then
					return false, err or 'connect has gone(5)'
				end
				arr[i] = string.sub(rsp,1,-3)   -- exclude \r\n
			elseif ld == 0 then  -- empty string
				arr[i] = ''
			else   -- nil obj
				arr[i] = nil
			end
		elseif tp == 4 then -- array
			if ld > 0 then
				local tb = {size=ld}
				rsp, err = _parseArr(id, tb)
				if not rsp then
					return false, err
				end
				arr[i] = tb
			elseif ld == 0 then  -- empty array
				arr[i] = {size=0}
			else   --  nil obj
				arr[i] = nil
			end
		elseif tp == 5 then -- Error
			arr[i] = false
		else  -- SimpleString or Integer
			arr[i] = ld
		end
	end
	return true
end

function r.new()
	local o = setmetatable({}, {__index=r})
	return o
end

function r:alive()
	return self._alive
end

function r:connect(arg)
	local host = arg.host
	local port = arg.port
	if not host or not port then
		error('redis.connect host or port not specify')
	end
	local auth = arg.auth
	local tmout = arg.timeout
	if not tmout then
		tmout = 10000
	end
	local id,err = sock.connect(host, port, {timeout=tmout}) 
	if not id then
		return false,err
	end
	self._alive = true
	self._id = id
	-- auth
	if auth then
		local rsp, err = self:call('AUTH', auth)
		if not rsp then
			return false, err
		end
	end
	return true
end

local function _readOneRsp(id)
	local rsp, err = _readLine(id,'\r\n')
	if not rsp then -- conn has gone
		return false, err or 'connect has gone(2)', true
	end
	local tp,ld = _parseLine(rsp)
	--print('recvRdsSvrRsp: ',tp, ld)
	if tp == 2 then  -- bulkstring
		if ld > 0 then   
			rsp, err = _readLen(id, ld+2)  -- include \r\n
			if not rsp then -- conn has gone
				return false, err or 'connect has gone(3)', true
			end
			return string.sub(rsp,1,-3)  -- exclude \r\n
		elseif ld == 0 then  -- empty string
			return ''
		else   -- nil obj
			return nil
		end
	elseif tp == 4 then -- array
		if ld > 0 then -- has item
			local arr = {size=ld}
			rsp, err = _parseArr(id, arr)
			if not rsp then  -- conn has gone
				return false,err,true
			end
			return arr
		elseif ld == 0 then  -- empty array
			return {size=0}
		end
		return nil  -- nil obj
	elseif tp == 5 then  -- server rsp error
		return false, ld
	end
	-- SimpleString or Integer, just return
	return ld
end

local function _call(self,cmd,...)
	if self._ppcnt then   -- in pipeline call
		_batAdd(cmd,...)
		self._ppcnt = self._ppcnt + 1
		return self
	else  -- normal call
		if not self._alive then
			return false, "connect not established"
		end
		local id = self._id
		-- send cmd
		local ok = _sendCmd(id,cmd,...)
		if not ok then
			self._alive = nil
			return false, 'connect has gone(1)'
		end
		-- read rsp
		local ret,err,disconn = _readOneRsp(id)
		if disconn then  -- conn has gone
			self._alive = nil
		end
		return ret, err 
	end
end

function r:call(cmd,...)
	return _call(self,cmd,...)
end

-- pipeline
function r:pipeline()
	if self._ppcnt then
		error('duplicate call of pipeline')
	end
	if not self._alive then
		return false, 'connect not established'
	end
	_batInit()
	self._ppcnt = 0
	return self
end
function r:sync()
	local batNum = self._ppcnt
	if not batNum then  -- pipeline has not been called
		error('pipeline has not been called')
	end
	if batNum == 0 then
		error('no cmd has add to pipeline')
	end
	-- clear status
	self._ppcnt = nil
	-- send
	local id = self._id
	local ret,err = _batSend(id._i,id._c)
	if ret < 1 then
		self._alive = nil
		return false, 'connect has gone(6)'
	end
	-- read rsp
	local arrRet = {size=batNum}
	local disconn
	for i=1,batNum do
		ret,err,disconn = _readOneRsp(id)
		if disconn then  -- conn has gone
			self._alive = nil
			return false, err
		end
		--print('dbg1: ', ret, arrRet.size)
		arrRet[i] = ret
	end
	return arrRet
end
--
function r:close()
	local id = self._id
	if id then
		sock.close(id)
		self._id = nil
	end
end

-- cmd wrap begin
-- normal cmd
function r:get(k)
	return _call(self,'GET',k)
end
function r:set(k,v)
	return _call(self,'SET',k,v)
end
function r:getset(k,v)
	return _call(self,'GETSET',k,v)
end
function r:setnx(k,v)
	return _call(self,'SETNX',k,v)
end
function r:setex(k,s,v)
	return _call(self,'SETEX',k,s,v)
end
function r:del(k)
	return _call(self,'DEL',k)
end
function r:exists(k)
	return _call(self,'EXISTS',k)
end
function r:expire(k,sec)
	return _call(self,'EXPIRE',k,sec)
end
function r:expireat(k,sec)
	return _call(self,'EXPIREAT',k,sec)
end
function r:keys(ptrn)
	return _call(self,'KEYS',ptrn)
end
function r:ttl(k)
	return _call(self,'TTL',k)
end
function r:incr(k)
	return _call(self,'INCR',k)
end
function r:incrby(k,i)
	return _call(self,'INCRBY',k,i)
end
function r:decr(k)
	return _call(self,'DECR',k)
end
function r:decrby(k,i)
	return _call(self,'DECRBY',k,i)
end
-- list cmd
function r:llen(k)
	return _call(self,'LLEN',k)
end
function r:lrange(k,start,stop)
	return _call(self,'LRANGE',start,stop)
end
function r:lpush(k,...)
	return _call(self,'LPUSH',k,...)
end
function r:lpop(k,cnt)
	return _call(self,'LPOP',k,cnt)
end
function r:rpush(k,...)
	return _call(self,'RPUSH',k,...)
end
function r:rpop(k,cnt)
	return _call(self,'RPOP',k,cnt)
end
function r:rpushlpop(src,dst)
	return _call(self,'RPUSHLPOP',src,dst)
end
function r:lrem(k,cnt,ele)
	return _call(self,'LREM',k,cnt,ele)
end
-- set cmd 
function r:sadd(k,...)
	return _call(self,'SADD',k,...)
end
function r:spop(k,cnt)
	return _call(self,'SPOP',k,cnt)
end
function r:sdiff(...)
	return _call(self,'SDIFF',...)
end
function r:sinter(...)
	return _call(self,'SINTER',...)
end
function r:sismember(k,m)
	return _call(self,'SISMEMBER',k,m)
end
function r:smembers(k)
	return _call(self,'SMEMBERS',k)
end
function r:smismember(k,...)
	return _call(self,'SMISMEMBER',k,...)
end
function r:smove(s,d,m)
	return _call(self,'SMOVE',s,d,m)
end
function r:srandmember(k,cnt)
	return _call(self,'SRANDMEMBER',k,cnt)
end
function r:srem(k,...)
	return _call(self,'SREM',k,...)
end
function r:sunion(...)
	return _call(self,'SUNION',...)
end
-- hash cmd
function r:hdel(k,...)
	return _call(self,'HDEL',k,...)
end
function r:hexists(k,f)
	return _call(self,'HEXISTS',k,f)
end
function r:hget(k,f)
	return _call(self,'HGET',k,f)
end
function r:hgetall(k)
	return _call(self,'HGETALL',k)
end
function r:hincrby(k,f,i)
	return _call(self,'HINCRBY',k,f,i)
end
function r:hkeys(k)
	return _call(self,'HKEYS',k)
end
function r:hlen(k)
	return _call(self,'HLEN',k)
end
function r:hmget(k,...)
	return _call(self,'HMGET',k,...)
end
function r:hmset(k,...)
	return _call(self,'HMSET',k,...)
end
function r:hrandfield(k,cnt,...)
	return _call(self,'HRANDFIELD',k,cnt,...)
end
function r:hset(k,f,v)
	return _call(self,'HSET',k,f,v)
end
function r:hsetnx(k,f,v)
	return _call(self,'HSETNX',k,f,v)
end
function r:hvals(k)
	return _call(self,'HVALS',k)
end
-- zset cmd
function r:zadd(k,...)
	return _call(self,'ZADD',k,...)
end


-- cmd wrap end

--
return r
