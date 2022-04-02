if not LPPS_C_LIB then
    LPPS_C_LIB = LPPS_OPEN_C_LIB() 
end
local _c = LPPS_C_LIB
local _loadfile = _c.stb_loadfile


local _s = {}
local _tbs = {}

function _s.loadfile(name)
	local tb,err = _s.query(name)
	if tb then  -- exist
		return tb
	end
	--
	tb,err = _loadfile(name)
	if not tb then
		error(err..': '..name)
	end
	_tbs[name] = tb  -- cache tableRoot
	return tb
end

function _s.query(name)
	return _tbs[name]
end

return _s
