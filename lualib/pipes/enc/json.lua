
if not LPPS_C_3RD_LIB then
    LPPS_C_3RD_LIB = LPPS_OPEN_C_3RD_LIB() 
end
local _c3rd = LPPS_C_3RD_LIB

local js = {}

function js.decode(str)
	return _c3rd.jsondec(str)
end
function js.encode(obj)
	return _c3rd.jsonenc(obj)
end

return js

