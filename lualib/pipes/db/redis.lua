
local pps = require('pipes')
local sock = require('pipes.socket')

local _cs = LPPS_C_SOCK_LIB
local _cSend = _cs.rdssend
local _readLine = sock.readline
local _readLen = sock.readlen
local _fork = pps.fork
--
local _batInit = _cs.rdsbatinit
local _batAdd = _cs.rdsbatadd
local _batSend = _cs.rdsbatsend
--
local _strSub = string.sub
local _strByte = string.byte


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
	local ld = _strByte(str)
	if not ld then -- str is empty
		error('redis rsp invalid(1): '..(str or 'nil'))
	end
	if ld == 43 then  --  + simple string 
		return 1, _strSub(str,2)
	elseif ld == 36 then  -- $  bulk string
		return 2, tonumber(_strSub(str,2))
	elseif ld == 58 then  -- :  integer
		return 3, tonumber(_strSub(str,2))
	elseif ld == 42 then  --  * array
		return 4, tonumber(_strSub(str,2))
	elseif ld == 45 then  --  - (minus) error 
		return 5, _strSub(str,2)
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
				arr[i] = _strSub(rsp,1,-3)   -- exclude \r\n
			elseif ld == 0 then  -- empty string
				rsp, err = _readLen(id, 2)  -- consume end \r\n
				if not rsp then
					return false, err or 'connect has gone(5-1)'
				end
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
	local o = setmetatable({}, {__index=r,__gc=r.close})
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
		local rsp, err = self:auth(auth)  --self:call('AUTH', auth)
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
	--print('willParseLine: ',rsp)
	local tp,ld = _parseLine(rsp)
	--print('recvRdsSvrRsp: ',tp, ld)
	if tp == 2 then  -- bulkstring
		if ld > 0 then   
			rsp, err = _readLen(id, ld+2)  -- include \r\n
			if not rsp then -- conn has gone
				return false, err or 'connect has gone(3)', true
			end
			return _strSub(rsp,1,-3)  -- exclude \r\n
		elseif ld == 0 then  -- empty string
			rsp, err = _readLen(id, 2)  -- consume tail \r\n
			if not rsp then -- conn has gone
				return false, err or 'connect has gone(3-1)', true
			end
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
		if self._subs then  -- in subs, do not readRsp here
			return ok
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

-- sub/pub
local function _startSubsRead(rdsObj)
	_fork(
		function(rds)
			local subs = rds._subs
			local chCbs = subs.chCbs
			local id = rds._id
			local ret,err,disconn
			while(true)
			do
				ret,err,disconn = _readOneRsp(id)
				if ret then -- rsp will not be nil in subscribe mode
					--print('retType: ',type(ret))
					local a1 = ret[1]
					if a1 then
						local a2 = ret[2]
						local b1,b2 = _strByte(a1)
						if b1 == 109 or b1 == 115 or b2 == 115 then -- 109:m, 115:s, message or subscribe or psubscribe
							chCbs[a2](a1,a2,ret[3])
						elseif b2 == 109 then -- pmessage
							chCbs[a2](a1,a2,ret[3],ret[4])
						elseif b1 == 117 or b2 == 117 then -- 117:u, unsubscribe or punsubscribe
							chCbs[a2](a1,a2,ret[3])
							chCbs[a2] = nil  -- rm cb for this channel
							if ret[3] == 0 then  -- subs over
								print('quit subscribe')
								rds._subs = nil
								break
							end
						else
							-- not channel msg(maybe pong), notify all cbs
							for ch,cb in pairs(chCbs) do 
								cb(a1,a2,ret[3])
							end
						end
					else   -- ret is rsp of quit, always OK
						rds._subs = nil
					end
				else
					if disconn then
						rds._alive = nil
						rds._subs = nil
						for ch,cb in pairs(chCbs) do
							cb('disconn',err)
						end
						break
					else  -- rdsSvr rsp error, notify all cbs
						for ch,cb in pairs(chCbs) do
							cb('error',err)
						end
					end
				end
			end	
		end,
		rdsObj)
end
local function _doSubscribe(isPsub,rds,cb,...)
	if rds._ppcnt then  -- in pipeline
		error('xsubscribe cant be called in pipeline')
	end
	local chs = {...}
	if #chs < 1 then
		error('no channel specify')
	end
	local ok
	if isPsub then
		ok = _sendCmd(rds._id,'PSUBSCRIBE',...)
	else
		ok = _sendCmd(rds._id,'SUBSCRIBE',...)
	end
	if not ok then  -- conn has gone
		return false, 'connect not established'
	end
	--
	local subs = rds._subs 
	if not subs then  -- subs init
		subs = {chCbs={}}
	end
	-- set cb of chs
	local chCbs = subs.chCbs
	for i,ch in ipairs(chs) do
		if chCbs[ch] then  -- duplicate reg for this channel
			error('duplicate xsubscribe for channel: '..ch)
		end
		chCbs[ch] = cb
	end
	if rds._subs then  -- sub has init before, just return
		return true
	end
	-- subs init(fork to read)
	rds._subs = subs
	_startSubsRead(rds)
	return true
end

function r:subscribe(cb,...)
	return _doSubscribe(false,self,cb,...)
end
function r:unsubscribe(...)
	return _call(self,'UNSUBSCRIBE')
end
function r:psubscribe(cb,...)
	return _doSubscribe(true,self,cb,...)
end
function r:punsubscribe(...)
	return _call(self,'PUNSUBSCRIBE')
end
function r:publish(ch,m)
	return _call(self,'PUBLISH',ch,m)
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
	if self._alive then
		self._alive = nil
		sock.close(self._id)
	end
end

-- cmd wrap begin
-- normal cmd
function r:auth(usr,pwd)
	if usr and pwd then
		return _call(self,'AUTH',usr,pwd)
	end
	return _call(self,'AUTH',usr)  -- only requirepass
end
function r:ping(m)
	if m then
		return _call(self,'PING',m)
	end
	return _call(self,'PING')
end
function r:quit()
	return _call(self,'QUIT')
end
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
	return _call(self,'LRANGE',k,start,stop)
end
function r:lpush(k,...)
	return _call(self,'LPUSH',k,...)
end
function r:rpop(k,cnt)
	if cnt then
		return _call(self,'RPOP',k,cnt)
	end
	return _call(self,'RPOP',k)
end
function r:lrem(k,cnt,ele)
	return _call(self,'LREM',k,cnt,ele)
end
function r:lmove(src,dst,dirSrc,dirDst)
	return _call(self,'LMOVE',src,dst,dirSrc,dirDst)
end
-- set cmd 
function r:sadd(k,...)
	return _call(self,'SADD',k,...)
end
function r:spop(k,cnt)
	if cnt then
		return _call(self,'SPOP',k,cnt)
	end
	return _call(self,'SPOP',k)
end
function r:sismember(k,m)
	return _call(self,'SISMEMBER',k,m)
end
function r:smembers(k)
	return _call(self,'SMEMBERS',k)
end
function r:smove(src,dst,m)
	return _call(self,'SMOVE',src,dst,m)
end
function r:srandmember(k,cnt)
	if cnt then
		return _call(self,'SRANDMEMBER',k,cnt)
	end
	return _call(self,'SRANDMEMBER',k)
end
function r:srem(k,...)
	return _call(self,'SREM',k,...)
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
function r:hset(k,...)
	return _call(self,'HSET',k,...)
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
-- script cmd
function r:eval(s,...)
	return _call(self,'EVAL',s,...)
end
function r:evalsha(s,...)
	return _call(self,'EVALSHA',s,...)
end
function r:scriptload(s)
	return _call(self,'SCRIPT LOAD',s)
end
function r:scriptexists(s,...)
	return _call(self,'SCRIPT EXISTS',s,...)
end
function r:scriptflush()
	return _call(self,'SCRIPT FLUSH')
end

-- cmd wrap end

--
return r
