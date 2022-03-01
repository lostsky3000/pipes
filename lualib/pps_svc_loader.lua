
local args = table.pack(...)  -- paramPtr, paramSize

local err = {}

--print('lua_path: ', LUA_PATH)
--print('lua_cpath: ', LUA_CPATH)
--print('src_path: ', SRC_PATH)

package.path = LUA_PATH
package.cpath = LUA_CPATH

local main, pattern
for pat in string.gmatch(LUA_PATH, "([^;]+);*") do
	local filename = string.gsub(pat, "?", SRC_PATH)
	local f, e = loadfile(filename)
	if not f then
		table.insert(err, e)
	else
		pattern = pat
		main = f
		break
	end
end

LUA_CPATH = nil
LUA_PATH = nil
SRC_PATH = nil

local pps = require('pipes')
local log = require('pipes.logger')
if not main then   -- load src failed
	local str = table.concat(err, "\n")
	log.error(str)
	error(str)
end


local ok, err
-- args: paramPtr, paramSize
if args.n > 1 then   -- has param
	--print('svcLoad, main, has param')
	--ok, ret = pcall(main, pps.unpackMsg(args[1], args[2]))
	ok,err = pps._exec(main, pps.unpack(args[1], args[2]))
else
	--print('svcLoad, main, no param')
	--ok, ret = pcall(main)
	ok,err = pps._exec(main)
end

--print('loaderDone: ok=',ok,', err=',err)
if not ok then
	log.error(err)
	error(err)
end


