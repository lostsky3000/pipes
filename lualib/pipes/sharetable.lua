if not LPPS_C_LIB then
    LPPS_C_LIB = LPPS_OPEN_C_LIB() 
end
local _c = LPPS_C_LIB
local _cLoadfile = _c.stb_loadfile
local _cQuery = _c.stb_query


local _s = {}
local _tbs = {}

function _s.loadfile(name,mode)
	local tb,err = _s.query(name)
	if tb then  -- exist
		return tb
	end
	--
	tb,err = _cLoadfile(name,mode)
	if not tb then
		error(err..': '..name)
	end
	_tbs[name] = tb  -- cache tableRoot
	return tb
end

function _s.query(name)
	local tb = _tbs[name]
	if not tb then  -- no cache, query from global
		tb = _cQuery(name)
	end
	if tb then
		_tbs[name] = tb
	end
	return tb 
end

return _s
