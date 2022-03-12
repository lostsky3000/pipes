local pps = require('pipes')
local log = require('pipes.logger')

pps.dispatch('error', 
function(err)
	log.error('sock_test1 caught err:'..err)
end)

local sock = require('pipes.socket')

local args = {...}

local sid = args[1]
local pid = args[2]
log.info('sock_test1 start')
--print('sock_test1, pid=', pid)


--pps.sleep(1000*10)

--[[
local msg,sz = sock.read(sid)
if msg then
	print('recv, msg='..msg..' sz='..sz)
	--pps.log('recv msg: ', msg, ' sz='..sz)
	sock.send(sid, 'echo from svr')
end
]]

pps.sleep(1000*6)

log.info('will read msg')

--[[]]
while(false)
do
	local msg,sz,trunc = sock.read(sid)
	--local msg,sz,trunc = sock.read(sid,3)
	--local msg,sz,trunc = sock.readlen(sid,5)
	--local msg,sz,trunc = sock.readline(sid,'ab')

	--print('msgType=', type(msg))
	if msg then
		local strLen = string.len(msg)
		print('recv, msg='..msg..' sz='..sz, 'strLen=',strLen, 'trunc=',trunc)
		--pps.log('recv msg=',msg,' sz=',sz)
		sock.send(sid, 'echo from svr,'..msg)
		if msg == 'close' then
			log.info('recv close')
			sock.close(sid)
		elseif msg == 'closesvr' then
			log.info('recv closesvr')
			sock.close(pid)
			print('close svr 2')
		elseif msg == 'shutdown' then
			log.info('recv shutdown')
			pps.shutdown()
		end
	else
		if sz then
			log.info('conn closed, has data: '..sz..', sz='..trunc)
		else
			log.info('conn closed, no data')
		end
		break
	end
end




