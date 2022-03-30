local pps = require('pipes')
local log = require('pipes.logger')

pps.dispatch('error', 
function(err)
	log.error('ws_test1 caught err:'..err)
end)

local ws = require('pipes.net.websocket')

local args = {...}

local sid = args[1]
local pid = args[2]
log.info('ws_test1 start')

pps.sleep(1000*2)

log.info('will read msg')

--[[]]
while(true)
do
	local msg,sz = ws.read(sid)
	--print('msgType=', type(msg))
	if msg then
		local strLen = string.len(msg)
		print('recv, msg='..msg..' sz='..sz, 'strLen=',strLen)
		--pps.log('recv msg=',msg,' sz=',sz)
		ws.sendtxt(sid, 'echo from svr,'..msg)
		ws.ping(sid)
		if msg == 'close' then
			log.info('recv close')
			ws.close(sid)
		elseif msg == 'closesvr' then
			log.info('recv closesvr')
			ws.close(pid)
			print('close svr 2')
		elseif msg == 'shutdown' then
			log.info('recv shutdown')
			pps.shutdown()
		end
	else
		if sz then  -- inner msg 
			log.info('inner msg: '..sz)
		else
			log.info('conn closed')
			break
		end
	end
end




