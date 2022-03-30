local pps = require('pipes')
local log = require('pipes.logger')
local sock = require('pipes.socket')

pps.dispatch('error', 
function(err)
	log.error('mysql_test1 caught err:'..err)
end)

log.info('mysql_test1 start')


pps.timeout(3000,function()
	pps.exit()
end)

local mysql = require('pipes.db.mysql')
local conn = mysql.new()


local host = 'ngj2-test-wan.rwlb.rds.aliyuncs.com' --'www.baidu.com'
local port = 3306

local ok,err = conn:connect(
	{host=host,port=port,
	 user='ngj2_w_df', pwd='askE%jb82@d#Y', db='ngj2_game_test'
	}
)

if ok then
	print('conn succ')
	--pps.sleep(6000)
	local sql = 'select * from t_test;select name from t_test'
	--sql = 'select 13'
	sql = 'start transaction'
	local rsNum,rs = conn:query(sql)
	if rsNum then
		print('query succ: ',rsNum,rs)
		--[[
		if rsNum == 1 then
			for i,r in ipairs(rs) do
				print('row'..i,r.name,r.age)
			end
		else
			for i,r in ipairs(rs) do
				print('rs'..i..':')
				for j,r2 in ipairs(r) do
					print('    row'..j,r2.name,r2.age)
				end
			end
		end
		]]
	else
		print('query err: ',rs)
	end
else
	print('conn failed: ', err)
end

