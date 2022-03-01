

local pps = require('pipes')
local log = require('pipes.logger')
--[[
pps.dispatch('error', 
function(err)
	--pps.log('test1 caught err:'..err)
	print('test1 caught err:'..err)
end)
]]

log.info('test_1 start')

pps.debug()

pps.dispatch('send', 
function(src, ...)
	print('recv send from: '..src..', tm='..os.time()..', args:', ...)
end
--[[
,function(data,sz)
	print('send unpack')
	return 'unpackRet'
end
--]]
)

--[[
pps.dispatch('call',
function(src, ...)
	local a = {...}
	print('recv call from: '..src..', tm='..os.time()..', args:', ...)
	local cmd = a[1]
	if cmd == 'noret' then
		
	elseif cmd == 'err' then
		error('callError test')
	else
		pps.ret('callRsp from test1')
	end
end

,function(data,sz)
	print('call unpack')
	return 'callUnpackArgs'
end
)
]]


--pps.sleep(1000)
--pps.exit()



