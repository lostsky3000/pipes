if not LPPS_C_3RD_LIB then
    LPPS_C_3RD_LIB = LPPS_OPEN_C_3RD_LIB() 
end
local _c = LPPS_C_3RD_LIB

local b = {}

function b.encodeSize(sz)
	return _c.b64encsz(sz)
end
function b.encode(str)
	return _c.b64enc(str)
end
function b.decode(str)
	return _c.b64dec(str)
end

return b

