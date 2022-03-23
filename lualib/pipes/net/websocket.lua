
--local pps = require('pipes')
local sock = require('pipes.socket')

local _cs = LPPS_C_SOCK_LIB
local _cSend = _cs.send

local ws = {}

local _read = sock._innerRead

function ws.server(port,args,cb)
	if not args then
		args = {}
	end
	if not cb or type(cb) ~= 'function' then
		error('ws.server, cb not specify')
	end
	args.protocol={type='websocket',uri=args.uri}
	return sock.listen(port, args, cb)
end

function ws.read(id)
	while(true)
	do
		local data,sz = _read(id,0)
		if data then  -- read succ
			return data, sz
		elseif not sz or sz < 1 then  -- closed
			return false
		end
		-- control frame
		if sz == 8 then  -- close
			sock.close(id)
		elseif sz == 10 then -- pong
			return false, 'PONG'
		end
	end
end

function ws.sendtxt(id,data,sz)
	local ret = _cSend(id._i,id._c,data,sz,1);
	if ret > 0 then
		return true
	end
	if ret == 0 then
		return false
	end
	error('invalid sendtxt: '..ret)
end
function ws.sendbin(id,data,sz)
	local ret = _cSend(id._i,id._c,data,sz,2);
	if ret > 0 then
		return true
	end
	if ret == 0 then
		return false
	end
	error('invalid sendtxt: '..ret)
end
function ws.ping(id)
	local ret = _cSend(id._i,id._c,data,sz,3);
	if ret > 0 then
		return true
	end
	if ret == 0 then
		return false
	end
	error('invalid ping: '..ret)
end
function ws.close(id)
	sock.close(id)
end


return ws
