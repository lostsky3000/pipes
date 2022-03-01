
local _c = LPPS_C_LIB
local _cLog = _c.log

local log = {}
local _lv = 0
function log.level(lv)
	if lv and type(lv) == 'number' and lv >= 0 and lv <= 3 then
		_lv = lv
	end
	return _lv
end
--
function log.debug(str,...)
	assert(str)
	if _lv <= 0 then
		_cLog(0,str,...)
	end
end
function log.info(str,...)
	assert(str)
	if _lv <= 1 then
		_cLog(1,str,...)
	end
end
function log.warn(str,...)
	assert(str)
	if _lv <= 2 then
		_cLog(2,str,...)
	end
end
function log.error(str,...)
	assert(str)
	if _lv <= 3 then
		_cLog(3,str,...)
	end
end

return log


