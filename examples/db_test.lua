
local pps = require('pipes')
local log = require('pipes.logger')

pps.dispatch('error', 
function(err)
	log.error('db_test caught err:'..err)
end)


log.info('db_test start')


pps.sleep(1000)

log.info('db_test1 111')

pps.exit()

