

if not LPPS_C_LIB then
    LPPS_C_LIB = LPPS_OPEN_C_LIB() 
end
local _c = LPPS_C_LIB

LPPS_MAX_INT = 2100000000

--print('pipesc: ', _c)

local logger = require('pipes.logger')
local _logE = logger.error

local pps = {}
--
local _co
--
local _def = require('pps_define')
local _cbTypes = _def.cbTypes
local _cbSend, _cbCall, _cbNet, _cbError, _upkSend, _upkCall

local _queue = require('pps_queue')
local _stack = require('pps_stack')
--
local _curCo   --, _curCoCtx
local _cosYield = {}
local _cosFree = _queue.new()
local _cosStack = _stack.new()

--
local _cFree = _c.free
local _cUnpackMsg = _c.unpackMsg
local _cSend = _c.send
local _cTmout = _c.timeout
local _cClock = _c.clock
local _cNow = _c.now
local _cSelfmsg = _c.selfmsg

local MTYPE_SVC_GONE = 3
local MTYPE_SVC_SELFMSG = 4
local MTYPE_NO_RET = 10
local MTYPE_LUA = 11
--
local _exit
local _debug


local function _pushCo(co)
	_cosStack.push(co)
	_curCo = co
end
local function _popCo()
	_cosStack.pop()
	_curCo = _cosStack.top()
end
local function _sendLua(to,session,...)
	return _cSend(to,session,MTYPE_LUA,...)
end
local function _onForkEnd(ok,ud)
	if _curCo._rsp then  -- need check response
		if not ud then
			ud = _curCo._ud
		end
		if not ud.ret then  -- is call req and not response
			_cSend(ud.src,-ud.ss,MTYPE_NO_RET)
		end
	end
	if not ok then
		_popCo()
	end
end
local function _coYield(key,...)
	if key then
		assert(not _cosYield[key])   -- debug
		_cosYield[key] = _curCo   -- rec cur co 
	end
	_popCo()
	return _co.yield(...)
end
local function _coResumeKey(key,...)
	local co = _cosYield[key]
	_cosYield[key] = nil
	_pushCo(co)
	local ok,err = _co.resume(co._co,...)
	if not ok then
		_onForkEnd(false)
	end
	return ok,err
end
local function _coResume(co,...)
	_pushCo(co)
	local ok,err = _co.resume(co._co,...)
	if not ok then
		_onForkEnd(false)
	end
	return ok,err
end

local _coMaxCnt = LPPS_MAX_INT - 1
local function _execArgs(fn1,ud1,args1)
	local co
	local coInfo
	while(true)
	do
		coInfo = _cosFree.pop()
		if not coInfo then  -- freequeue is empty
			break
		end
		if coInfo.cnt < _coMaxCnt then  -- can reuse
			break
		end
	end
	if coInfo then 
		co = coInfo.co
	else   -- no free co
		coInfo = {}
		co = _co.create(function(fn,ud,args)
			local coCtx = {_co=coInfo.co}
			local cnt = 1
			while(true)
			do
				coCtx._cnt = cnt
				coInfo.cnt = cnt
				cnt = cnt + 1
				coCtx._ud = ud
				if ud and ud.tp == 1 then
					coCtx._rsp = true
				else 
					coCtx._rsp = nil
				end
				coCtx._usrwait = nil  -- user wait flag
				_pushCo(coCtx)   -- never fail
				fn(table.unpack(args))  -- perhaps error
				_onForkEnd(true, ud)  -- never fail
				--
				if cnt < _coMaxCnt then  -- can reuse
					_cosFree.push(coInfo)  -- never fail
					fn,ud,args = _coYield()  -- never fail
				else
					coCtx._cnt = cnt
					coInfo.cnt = cnt
					_popCo()
					break
				end
			end
		end)
		coInfo.co = co
	end
	if _debug then
		--print('dbg_1 coStackLen = ',_cosStack.size(), 'cosFreeLen=',_cosFree.size())  -- debug
	end
	local ok,err = _co.resume(co,fn1,ud1,args1)
	if not ok then -- fn() error
		_onForkEnd(false,ud1)
	end
	if _debug then
		--print('dbg_2,ret=',ok, ', coStatus='.._co.status(co)..', coStackLen = ',_cosStack.size(), 'cosFreeLen=',_cosFree.size(),'\n')  -- debug
	end
	return ok,err 
	--return _co.resume(co,fn1,ud1,args1)
end
local function _exec(fn,ud,...)
	local args = {...}
	return _execArgs(fn,ud,args)
end

function pps.dispatch(tp,cb,upk)
	local tpc = _cbTypes[tp]
	if not tpc then
		error('type not supported: '..tp)
	end
	if tpc == _cbTypes.send and not _cbSend then
		_cbSend = cb
		if upk then
			_upkSend = upk
		end
		return
	end
	if tpc == _cbTypes.call and not _cbCall then
		_cbCall = cb
		if upk then
			_upkCall = upk
		end
		return
	end
	if tpc == _cbTypes.net and not _cbNet then
		_cbNet = cb
		return
	end
	if tpc == _cbTypes.error and not _cbError then
		_cbError = cb
		return
	end
	error('type duplicate dispatched: '..tp)
end

function pps.send(to,...)
	return _sendLua(to,0,...)
end
--
local _maxCallId = LPPS_MAX_INT - 1
local _callIdCnt = 1
local _callRetUpks = {}
local function _callLua(to,upk,...)
	local ok
	local callId = _callIdCnt
	if upk and type(upk) == 'function' then
		ok = _sendLua(to,callId,...)
		if ok then
			_callRetUpks[callId] = upk
		end
	else
		ok = _sendLua(to,callId,upk,...)
	end
	if not ok then
		return false, 'service not exist'
	end
	local loopCnt = LPPS_MAX_INT
	while (true)
	do
		if _callIdCnt >= _maxCallId then
			_callIdCnt = 1
		else
			_callIdCnt = _callIdCnt + 1
		end
		if not _cosYield['l'.._callIdCnt] then
			break
		end
		loopCnt = loopCnt - 1
		if loopCnt < 1 then
			error('no more callId to alloc')
		end
	end
	return _coYield('l'..callId)
end

function pps.call(to,upk,...)
	return _callLua(to,upk,...)
end

function pps.ret(...)
	local ctx = _curCo._ud
	if not ctx or not ctx.tp or ctx.tp ~= 1 then
		error('ret() cant be called here')
	end
	if ctx.ret then
		error('ret() has been called')
	end
	_sendLua(ctx.src,-ctx.ss,...)
	ctx.ret = true
end

-- time fn begin
local _scheds = {}
local _tmouts = {}
local _maxTmId = LPPS_MAX_INT - 1
local _tmIdCnt = 1
local function _genTmId()
	local tmId = _tmIdCnt
	local loopCnt = LPPS_MAX_INT
	while (true)
	do
		if _tmIdCnt >= _maxTmId then
			_tmIdCnt = 1
		else
			_tmIdCnt = _tmIdCnt + 1
		end
		if not _cosYield['t'.._tmIdCnt] and not _scheds[_tmIdCnt] then
			break
		end
		loopCnt = loopCnt - 1
		if loopCnt < 1 then
			error('no more tmId to alloc')
		end
	end
	return tmId
end
function pps.sleep(ms)
	local tmId = _genTmId()
	_cTmout(ms,tmId,0)
	local key = 't'..tmId
	_curCo._usrwait = key
	return _coYield(key)
end
function pps.timeout(ms,cb)
	local tmId = _genTmId()
	_cTmout(ms,tmId,0)
	_tmouts[tmId] = {cb=cb}
	return {_id=tmId}
end
function pps.schedule(ms,cb)
	local tmId = _genTmId()
	_cTmout(ms,tmId,100)
	_scheds[tmId] = {cb=cb,a=true,ms=ms}
	return {_id=tmId}
end
function pps.unschedule(id)
	local sched = _scheds[id._id]
	if sched then
		sched.a = nil
	end
end
function pps.clock()
	return _cClock()
end
function pps.now()
	return _cNow()
end
-- time fn end

--
function pps.yield()
	local ss = _genTmId()
	local ok = _cSelfmsg(MTYPE_SVC_SELFMSG,ss,2)
	if not ok then
		error('pps.yield() ccall failed')
	end
	return _coYield('t'..ss)
end
function pps.fork(fn,...)
	local ss = _genTmId()
	local ok = _cSelfmsg(MTYPE_SVC_SELFMSG,ss,1)
	if not ok then
		error('pps.fork() ccall failed')
	end
	_cosYield['t'..ss] = {fn=fn,arg={...}}
end
function pps.corunning()
	local co = {_raw=_curCo, _cnt=_curCo._cnt}
	return co
end
function pps.wait()
	if _curCo._usrwait then
		error('wait() status invalid')
	end
	_curCo._usrwait = 'wait'
	return _coYield()
end
local _sleepBreak = {}
function pps.wakeup(coUsr)
	local co = coUsr._raw
	if coUsr._cnt ~= co._cnt then -- co has been droped or reused
		return false
	end
	local key = co._usrwait
	if not key then   -- not inWait or inSleep
		--error('wakeup() status invalid')
		return false
	end
	co._usrwait = nil
	local ok,err
	if key == 'wait' then
		ok,err = _coResume(co)
	else
		_sleepBreak[key] = true
		ok,err = _coResume(co,false,'BREAK')
	end
	return true,ok,err
end

function pps.debug()
	_debug = true
end

--
function pps.newservice(src,...)
	local id = _c.newservice(src,1,...)
	return id
end
function pps.exclusive(src,...)
	local id = _c.newservice(src,2,...)
	return id
end
function pps.unpack(ptr,sz)
	return _cUnpackMsg(ptr,sz)
end
function pps.free(ptr)
	_cFree(ptr)
end

function pps.exit()
	assert(not _exit)
	local r = _c.exit()
	_exit = true
	_coYield()
end
function pps.id()
	return _c.id()
end
function pps.shutdown()
	_c.shutdown()
end

--
function pps._yield(key,...)
	if not key then
		error('pipes._yield no key specified')
	end
	return _coYield('x'..key, ...)
end
function pps._resumeKey(key,...)
	if not key then
		error('pipes._resume no key specified')
	end
	return _coResumeKey('x'..key, ...)
end
function pps._exec(fn,...)
	return _exec(fn,nil,...)
end
function pps._procError(err)
	assert(not _exit) -- debug 
	_logE(err)
	if _cbError then
		_exec(_cbError,nil,err)
	end
end

-- reg lua msg cb 
_c.dispatch(1,
function(src,ss,tp,data,sz)
	--print('dispatch recv msg, src='..src..', session='..ss)
	if ss == 0 then  -- send 
		if not _cbSend then 
			_cFree(data)
			return 
		end
		local ok,err = _exec(function()
			if _upkSend then
				_cbSend(src, _upkSend(data,sz))
			elseif data then
				_cbSend(src, _cUnpackMsg(data,sz))
			else
				_cbSend(src)
			end
		end)
		if data then
			_cFree(data)
		end
		if not ok then
			pps._procError(err)
		end
	elseif ss > 0 then  -- call req 
		if not _cbCall then --no call cb specified
			_cFree(data)
			_cSend(src,-ss,MTYPE_NO_RET)  -- never error
			return 
		end
		local callCtx = {src=src,ss=ss,tp=1}
		local ok,err = _exec(function()
			if _upkCall then
				_cbCall(src, _upkCall(data,sz))
			elseif data then
				_cbCall(src, _cUnpackMsg(data,sz))
			else
				_cbCall(src)
			end
		end, callCtx)
		if data then
			_cFree(data)
		end
		if not ok then
			pps._procError(err)
		end
	else  -- call rsp
		local ok,err 
		ss = -ss
		if tp ~= MTYPE_SVC_SELFMSG then  -- normal call rsp
			--print('lua recv callRsp, tp='..tp..', ss='..ss)
			local upk = _callRetUpks[ss]
			if upk then
				_callRetUpks[ss] = nil
			end
			if tp == MTYPE_NO_RET then
				ok,err = _coResumeKey('l'..ss, false, 'no resp from '..src)
			elseif tp == MTYPE_SVC_GONE then
				ok,err = _coResumeKey('l'..ss, false, 'dst service may has gone: '..src)
			else
				if upk then
					ok,err = _coResumeKey('l'..ss, true, upk(data,sz))
				elseif data then
					ok,err = _coResumeKey('l'..ss, true, _cUnpackMsg(data,sz))
				else
					ok,err = _coResumeKey('l'..ss, true)
				end
			end
			if data then
				_cFree(data)
			end
		else  -- selfmsg 
			if src == 1 then  -- fork cb
				local key = 't'..ss
				local ctx = _cosYield[key]
				_cosYield[key] = nil
				ok,err = _execArgs(ctx.fn, nil, ctx.arg)
			elseif src == 2 then   -- yield cb 
				ok,err = _coResumeKey('t'..ss)
			else 
				-- sth wrong
			end
		end
		if not ok then
			pps._procError(err)
		end
	end
end)

-- reg timer msg cb 
_c.dispatch(2,
function(ss,left,now)
	ss = -ss
	--print('lua recv timer msg, coResume, key='..'t'..ss)
	local ok,err 
	local sched = _scheds[ss]
	if sched then  -- is schedule
		if sched.a then  -- schedule continue
			if left == 0 then
				_cTmout(sched.ms,ss,100)
			end
			ok,err = _exec(sched.cb, nil, now)
		else  -- unschedule
			if left == 0 then  -- remove
				print('schedule removed')  -- debug
				_scheds[ss] = nil
			end
			ok = true
		end
	else
		local tmo = _tmouts[ss]
		if tmo then  -- timeout
			ok,err = _exec(tmo.cb, nil, now)
		else  -- sleep
			local key = 't'..ss
			local co = _cosYield[key]
			if _sleepBreak[key] then
				--print('sleep timeup, but co has break')  -- debug
				_sleepBreak[key] = nil
				_cosYield[key] = nil
				ok = true
			else
				co._usrwait = nil
				ok,err = _coResumeKey(key, true)
			end
		end
	end
	if not ok then
		pps._procError(err)
	end
end)


-- rewrite sysapi
_co = coroutine
coroutine = nil
--

return pps



