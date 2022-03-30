local pps = require('pipes')
local log = require('pipes.logger')

pps.dispatch('error', 
function(err)
	log.error('ws_test2 caught err:'..err)
end)

local ws = require('pipes.net.websocket')

local args = {...}

local sid = args[1]
local pid = args[2]
log.info('ws_test2 start')

pps.sleep(1000*3)

log.info('will read msg')


local function _doRead(id)
	local msg,sz = ws.read(id)
	if msg then
		print('recvMsg: ', msg)
	end
end

pps.timeout(1000, 
function()
	print('tmout 1, doReadBegin')
	_doRead(sid)
	print('tmout 1, doReadEnd')
end)

pps.timeout(3000, 
function()
	print('tmout 2, doReadBegin')
	_doRead(sid)
	print('tmout 2, doReadEnd')
end)

pps.timeout(5000, 
function()
	print('tmout 3, doReadBegin')
	_doRead(sid)
	print('tmout 3, doReadEnd')
end)



