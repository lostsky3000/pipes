local pps = require('pipes')
local log = require('pipes.logger')
pps.dispatch('error', 
function(err)
	log.error('sock_test1 caught err:'..err)
end)

local sock = require('pipes.socket')

log.info('sock_test2 start')


local host = '192.168.123.7' --'www.baidu.com'
local port = 80
log.info('will connect '..host..':'..port)
local id,err = sock.connect(host, port, {_timeout=5000})
if id then
	log.info('conn succ')
	sock.send(id, 'GET /index HTTP/1.1 \r\n')
	while(true)
	do
		local msg,sz = sock.read(id)
		if msg then
			print('recv: ', msg)
		else
			log.info('conn lost')
			break
		end
	end
else
	log.info('conn failed, err='..err)
end


