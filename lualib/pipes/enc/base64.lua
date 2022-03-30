if not LPPS_C_3RD_LIB then
    LPPS_C_3RD_LIB = LPPS_OPEN_C_3RD_LIB() 
end
local _c = LPPS_C_3RD_LIB
local _enc = _c.b64enc
local _dec = _c.b64dec

local b = {}

function b.encodeSize(sz)
	return _c.b64encsz(sz)
end
function b.encode(str)
	return _enc(str)
end
function b.decode(str)
	return _dec(str)
end

return b

