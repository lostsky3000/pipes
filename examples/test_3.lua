local pps = require('pipes')
local log = require('pipes.logger')

pps.dispatch('error', 
function(err)
	print('test3 caught err:'..err)
end)

log.info('test3 start')

pps.sleep(1000)


local function fn3()
	log.info('fn3 start')
	--pps.exit()
	log.info('fn3 end')
end
local function fn2()
	log.info('fn2 start')
	pps.fork(fn3)
	log.info('fn2 end')
end
local function fn1()
	log.info('fn1 start')
	pps.fork(fn2)
	log.info('fn1 end')
end
log.info('will fork')
local ok,err = pps.fork(fn1)
log.info('all fork done')


pps.sleep(2000)

local co = {}
pps.fork(
function()
	co.co = pps.corunning()
	log.info('will wait')
	
	--pps.wait()
	local ok,ret = pps.sleep(2000)
	if ok then
		log.info('sleep done by timeout')
	else
		log.info('sleep done by wakeup: ',ret)
	end
	log.info('wait done')
end
)
log.info('will sleep')
pps.sleep(1000)
log.info('will wakeup')
pps.wakeup(co.co)
log.info('wakeup done')


pps.sleep(3000)
log.info('yield test 1')
pps.fork(
function(num)
	log.info('fork cb 1')
	pps.yield()
	log.info('fork cb 2')
end,123
)
pps.yield()
log.info('yield test 2')


