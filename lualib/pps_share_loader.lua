
package.path = LUA_PATH

--print('luaPath: ', LUA_PATH)

local tb
if MODE then  -- specify file format
	if MODE == 'json' then -- json file
		local file,err
		for pat in string.gmatch(LUA_PATH, "([^;]+);*") do
			local filePath = string.gsub(pat, "?.lua", TABLE_NAME)
			--print('filePath: ', filePath)
			file,err = io.open(filePath,'r')
			if file then
				break
			end
		end
		if not file then
			error('open sharefile failed: '..(err or ''))
		end
		local str,sz = file:read('a')
		file:close()
		-- from json to luaTable
		local json = require('pipes.enc.json')
		tb,err = json.decode(str)
		if not tb then
			error('parse sharefile error: '..err)
		end
	else
		error('unsupport sharefile format: '..MODE)
	end
else
	tb = require(TABLE_NAME)
end

LUA_PATH = nil
TABLE_NAME = nil
MODE = nil

return tb

