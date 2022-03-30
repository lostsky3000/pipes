
if not LPPS_C_3RD_LIB then
    LPPS_C_3RD_LIB = LPPS_OPEN_C_3RD_LIB() 
end
local _c = LPPS_C_3RD_LIB
local _enc = _c.jsonenc
local _dec = _c.jsondec

local js = {}

function js.encode(obj)
	return _enc(obj)
end
function js.decode(str)
	return _dec(str)
end

return js

