
local pps = require('pipes')
local log = require('pipes.logger')

pps.dispatch('error', 
function(err)
	log.error('test2 caught err:'..err)
end)

log.info('test_2 start')
--pps.debug()

--pps.sleep(2000)
--error('test_2 error test')

--[[
2 - 9
3 - 8
4 - 7
5 - 6
6 - 5
7 - 4
8 - 3
9 - 2

2 + 7  (11-2*2)
3 + 5  (11-3*2)
4 + 3  (11-4*2)
5 + 1  (11-5*2)
6 + -1 (11-6*2)
7 + -3 (11-7*2)

n + (11 - n*2)

]]


pps.sleep(1000)
local test1 = pps.newservice('test_1')
pps.send(test1, 123,'hehe')


pps.sleep(1000)
--pps.log('test_2 will call test_1')
local ret,data = pps.call(test1, 'noret', 321)
ret,data = pps.call(test1, 'err', 123)
ret,data = pps.call(test1, 'normal', 123)
print('test2 callRet: ',ret,data)
--[[
pps.sleep(1000)
print('test2 will call again')
ret,data = pps.call(test1, 'err2', 456)
--print('recvCallret: ', ret, data)
pps.sleep(2000)
pps.send(test1, 'ddddd')

]]
