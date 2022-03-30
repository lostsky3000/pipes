
local pps = require('pipes')
local log = require('pipes.logger')
pps.dispatch('error', 
function(err)
	log.error('boot caught err:'..err)
end)

local sock = require('pipes.socket')

log.info('boot service start', true, 123, 88.99)

--error('boot error test')
--local n = table.unpack(nil)

------local test2 = pps.newservice('mysql_test1')


local function dumpTable(tb)
	for k,v in pairs(tb) do
		if type(v) == 'table' then
			print(k..':')
			dumpTable(v)
		else
			print(k..'=',v)
		end
	end
end
-- json test
local json = require('pipes.enc.json')
local str = '{"name":"dada", "age":25, "done":true, "fav":[{"nm":"sport"},{"nm":"tv"}]}'
local obj = json.decode(str)
print('jsonDec: ',obj, #obj.fav)

obj = {name='dada', age=25, fav={111,nil,222,"hehe"}}
str = json.encode(obj)
print(str)

--dumpTable(obj)
--local test3 = pps.newservice('test_3')

--local db1 = pps.exclusive('db_test')
--pps.sleep(2000)
--local db2 = pps.exclusive('db_test')
--[[
pps.sleep(1000)

pps.log('start timeout call')
pps.timeout(2000, function()
	pps.log('on timeout')
end)

pps.sleep(2200)
pps.log('start yield')
pps.yield()
pps.log('yield done')
]]


--pps.newservice('sock_test2')


--[[ listen test --]]
local port = 10086
--log.info('prepare to listen at ',port)


--[[
local id,err = sock.listen(port, 
{backlog=64
--bind={'192.168.0.24','127.0.0.1'}
,bind={'0.0.0.0'}
,protocol={type='websocket',uri='test'}
},
function(id,idListen) 
	--local host, port = sock.remote(id)
	--pps.log('connIn: ',host,':',port)
	--pps.sleep(2000)
	--pps.log('prepare to close sock')
	--sock.close(id)
	local idSvc = pps.newservice('sock_test1',id,idListen)
end)
]]


--[[  
print("maxInt: ",math.maxinteger)

local ws = require('pipes.net.websocket')
local id,err = ws.server(port,
{uri='test'
--,backlog=64
--,bind={'0.0.0.0'}
},
function(id,idListen) 
	local host, port = sock.remote(id)
	log.info('connIn: ',host,':',port)
	--pps.sleep(2000)
	--pps.log('prepare to close sock')
	--sock.close(id)
	local idSvc = pps.newservice('ws_test2',id,idListen)
end)
if id then
	log.info('listen succ')
else
	log.info('listen failed: ',err)
end
]]


--[[ 

--pps.sleep(1000*1)
local redis = require('pipes.db.redis')
local rds = redis.new()

print('rds: ',rds)

local function dumpRsp(rsp)
	if type(rsp) == 'table' then
		for i=1,rsp.size do
			print('rsp('..i..'): ', rsp[i])
		end
	else
		print('rsp: ', rsp)
	end
end

local rsp,err = rds:connect({
	host='47.103.91.7',
	port=25002,
	auth='dy0{t%@JFr!^Y]i'
})
if rsp then
	print('connRds succ')
	--rsp,err = rds:call('del','test1')
	--rsp,err = rds:call('HMSET','test1','name','dada','age',25)
	--rsp,err = rds:call('HGETALL','test1')
	--rsp,err = rds:pipeline()
				--:call('SET', 'test1', 'dada1')
				--:call('GET', 'test1')
				--:call('DEL', 'test1')
				--:set('test1', 'data1')
				--:get('test1')
				--:del('test1')
				--:sync()
	
	rsp,err = rds:subscribe(
		function(tp,ch,data) 
			--tp: disconn(conn gone), subscribe, unsubscribe, message, pong, error(svrRspErr)
			print('onPubMsg: ',tp,ch,data)
			if tp == 'message' then
				if data == 'ping' then
					rds:ping()
				elseif data == 'invalid' then
					rds:get('dddd')
				elseif data == 'unsub' then
					rds:unsubscribe()
				elseif data == 'quit' then
					rds:quit()
				end
			end
		end
		,'test_ch1')
	if rsp ~= false then
		print('callRdsSucc: ',rsp)
		--print(rsp,ch,m)
		dumpRsp(rsp)
	else
		if rds:alive() then
			print('callRdsErr(svrRspErr): ',err)
		else
			print('callRdsErr(connGone): ',err)
		end
	end
else
	print('connRds failed: ',err)
end
]]

--[[
pps.log('sock test: ', sock.test())
--]]


--[[ 
local svcNum = 10
local tb = {}
for i=1, svcNum do
	local id = pps.newservice('test_2',i)
	table.insert(tb,id)
end
--]]

--[[ schedule test 
local schCnt = 1
pps.schedule(100, 
function(now)
	pps.log('boot schedule, cnt='..schCnt..', now='..pps.now()..', clk='..now)
	schCnt = schCnt + 1
end)
--]]



--pps.error('trig err test')
--[[
print('boot before delay', pps.clock())
--pps.sleep(1000)
local ret = pps.sleep(1000)
print('boot after dealy', pps.clock())

print('will create test_1,', pps.clock())
local id = pps.newservice('test_1', 'hehe')

-- send test

for i=1, 3 do
	print('boot will send(1) '..i)
	pps.send(id, i, i*5)
end
for i=4, 7 do
	pps.sleep(1000)
	print('boot will send(2) '..i)
	pps.send(id, i, i*5)
end
--]]


-- schedule test begin
--[[
local schCnt = 0
local sch = {}
sch.id = pps.schedule(100, 
function(now)
	print('boot schedule cb, now='..now..', realNow='..pps.clock()..', cnt='..schCnt)
	
	if schCnt == 1553 then
		print('boot unschedule')
		pps.unschedule(sch.id)
	end
	schCnt = schCnt + 1

end)
--]]
-- schedule test end


-- call test 
--[[
print('boot will call, tm='..os.time())
print('boot recv callRet: ', pps.call(id, 'callReq', 123)) 
--]]

--[[
-- queue test begin
local queue = require('pps_queue')
local q = queue.new()
for i=1,10 do
	q.push({num=i})
	print('push, qSz='..q.size())
end
local e = q.pop()
while (e)
do
	print('pop, num='..e.num..', qSz='..q.size())
	e = q.pop()
end
-- queue test end

-- seq-queue test begin
local squeue = require('pipes.queue')
local cs1 = squeue()
local cs2 = squeue()
for i=1,5 do
	cs1(function()
		print('cs1 fn, i=', i)
	end)
end
print('========')
for i=1,2 do
	cs2(function()
		print('cs2 fn, i=', i)
	end)
end
-- seq-queue test end
--]]


--print('luaExit: ', pps.exit(), os.time())

--pps.newservice('2ndlua')

