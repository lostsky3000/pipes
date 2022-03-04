
local pps = require('pipes')
local _logger = require('pipes.logger')
local _logE = _logger.error
if not LPPS_C_LIB then
    LPPS_C_LIB = LPPS_OPEN_C_LIB() 
end
local _c = LPPS_C_LIB
if not LPPS_C_SOCK_LIB then
    LPPS_C_SOCK_LIB = LPPS_OPEN_C_SOCK_LIB() 
end
local _cs = LPPS_C_SOCK_LIB

if not _cs.hasnet() then   -- net not available
	error('net mod not available')
	return nil
end
_cs.init()

--
local s = {}

local _callsYield = {}
local _exec = pps._exec
local _yield = pps._yield
local _resumeKey = pps._resumeKey
local function _coYield(key,ctx,...)
	_callsYield[key] = ctx
	return _yield(key,...)
end
local function _coResumeKey(key,...)
	_callsYield[key] = nil
	return _resumeKey(key,...)
end

--
local _maxCallId = math.maxinteger - 1
local _callIdCnt = 1
local function _genCallId()
	local callId = _callIdCnt
	local loopCnt = math.maxinteger
	while (true)
	do
		if _callIdCnt >= _maxCallId then
			_callIdCnt = 1
		else
			_callIdCnt = _callIdCnt + 1
		end
		if not _callsYield['s'.._callIdCnt] then
			break
		end
		loopCnt = loopCnt - 1
		if loopCnt < 1 then
			error('no more sockCallId to alloc')
		end
	end
	return callId
end

local _listens = {}
function s.listen(port, args, cb)
	-- session, port, backlog, sendBuf, recvBuf, addrs
	if not args then
		error('socket.listen args #2 not specify')
	end
	local tp = type(args)
	if tp == 'function' then  -- args not specify
		cb = args
		args = {}
	elseif tp == 'table' then   -- check cb
		if not cb or type(cb) ~= 'function' then
			error('socket.listen callback not specify')
		end
	else 
		error('socket.listen args type invalid: '..tp) 
	end
	local ss = _genCallId()
	local ok,err
	local bind = args.bind
	if not bind then
		ok, err = _cs.listen(ss,port,args.backlog,args.sendbuf,args.recvbuf,args.protocol)
	elseif type(bind) == 'table' then
		ok, err = _cs.listen(ss,port,args.backlog,args.sendbuf,args.recvbuf,args.protocol,table.unpack(bind))
	else
		error('socket.listen bind list invalid')
	end
	if not ok then
		return false, 'listen failed: '..err
	end
	return _coYield('s'..ss, {cb=cb,port=port})
end

function s.connect(host,port,args)
	-- session, host, port, timeout, sendBufLen, recvBufLen
	if not args then
		args = {}
	end
	local tmout = args.timeout
	if not tmout then  -- specify default timeout
		tmout = 30000
	end
	local ss = _genCallId()
	_cs.connect(ss,host,port,tmout,args.sendbuf,args.recvbuf)
	return _coYield('s'..ss)
end

function s.remote(id)
	return _cs.addr(id._i,id._c)
end

function s.close(id)
	local idx = id._i
	local cnt = id._c
	local key = idx..'_'..cnt
	_listens[key] = nil   -- remove listenfd if exist
	_cs.close(idx,cnt)
end

local function _read(id,dec,...)
	local ss = _genCallId()
	local data,sz,conti,trunc = _cs.read(id._i,id._c,ss,dec,...)
	if data then  -- read sth
		if conti then
			return data,sz,trunc 
		else     -- last read
			return false,data,sz
		end
	end
	if sz == 0 then  -- read wait
		return _coYield('r'..ss, true)
	elseif sz == -1 then   -- sock has gone
		return false
	elseif sz == -2 then   -- multi read 
		error('concurrent read not allowed')
	elseif sz == -3 then   -- 
		error('last read not complete')
	end
	error('invalid read: '..sz)
end
function s.read(id,max)
	if not max then
		max = 65536
	end
	return _read(id,0,max)
end
function s.readlen(id,len)
	return _read(id,1,len)
end
function s.readline(id,sep,max)
	if not sep then
		sep = '\n'
	end
	if not max then
		max = 65536
	end
	return _read(id,2,sep,max)
end
function s.send(id,data,sz)
	local ret = _cs.send(id._i,id._c,data,sz);
	if ret > 0 then
		return true
	end
	if ret == 0 then
		return false
	end
	error('invalid send: '..ret)
end


function s.test()
	return _cs.test()
end


-- reg net msg cb 
_c.dispatch(3,
function(cmd,ss,...)
	if cmd == 5 then  -- read wait ret
		-- cmd,data, ss,sz,conti,trunc
		local tb = {...}
		local ok,err
		if ss then  -- has data
			if tb[3] then   -- normal read 
				ok,err = _coResumeKey('r'..tb[1], ss, tb[2], tb[4])
			else   -- last read 
				ok,err = _coResumeKey('r'..tb[1], false, ss, tb[2])
			end
		else  -- no data, sock has gone
			ok,err = _coResumeKey('r'..tb[1], false)
		end
		if not ok then
			pps._procError(err)
		end
	elseif cmd == 2 then   -- tcp conn in: ss=idxParent
		local tb = {...}   -- cntParent,idxSock,cntSock
		local pidx = ss
		local pcnt = tb[1]
		local lsn = _listens[pidx..'_'..pcnt]
		local id = {_i=tb[2], _c=tb[3]}
		if not lsn then  -- listen has close, close this sock
			s.close(id)
			return 
		end
		local idParent = {_i=pidx,_c=pcnt}
		local ok,err = _exec(lsn._cb, id, idParent)
		if not ok then
			pps._procError(err)
		end
	elseif cmd == 8 then  -- conn ret 
		ss = -ss
		local key = 's'..ss
		local tb = {...}  -- ret
		local connRet = tb[1]
		local ok,err
		if connRet == 0 then   -- conn succ
			ok,err = _coResumeKey(key, {_i=tb[2], _c=tb[3]})
		else
			ok,err = _coResumeKey(key, false, connRet)
		end
		if not ok then
			pps._procError(err)
		end
	elseif cmd == 1 then  -- listen ret
		ss = -ss
		local key = 's'..ss
		local ctx = _callsYield[key]
		if not ctx then  -- warning
			return
		end
		local ok,err
		local tb = {...}  -- ret, (sockIdx, sockCnt) or (errCode)
		if tb[1] then  -- listen succ
			local idx = tb[2]
			local cnt = tb[3]
			local sockKey= idx..'_'..cnt
			local sockId = {_i=idx,_c=cnt,_key=sockKey}
			_listens[sockKey] = {_cb=ctx.cb,_port=ctx.port,_id=sockId}
			ok,err = _coResumeKey(key, sockId)
		else  -- listen failed
			ok,err = _coResumeKey(key, false, 'server failed: '..tb[2])
		end
		if not ok then
			pps._procError(err)
		end
	end
end)

return s

