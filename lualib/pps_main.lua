
local boot = require "pipesc.boot"
local pps = require "pipesc"

local config = {}

local function init_config()
	local configFile = assert(arg[1])
	assert(loadfile(configFile, "t", config))()
	assert(config.boot_service)
	assert(config.service_root)

	config.lua_path = './lualib/?.lua;'..config.service_root..'/?.lua;'..(config.lua_path or package.path)
	config.lua_cpath = config.lua_cpath or package.cpath
end 

local function reg_close(f)
	return setmetatable({}, {__close=f})
end

init_config()
boot.init(config)

local _<close> = reg_close(boot.deinit)

boot.run()

