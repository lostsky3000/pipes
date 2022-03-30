
if not LPPS_C_3RD_LIB then
    LPPS_C_3RD_LIB = LPPS_OPEN_C_3RD_LIB() 
end
local _c = LPPS_C_3RD_LIB
local _sha1 = _c.sha1

local ec = {}

function ec.sha1(str)
	return _sha1(str)
end

return ec

