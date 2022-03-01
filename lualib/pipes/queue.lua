

local queue = require('pps_queue')

local function newQueue()	
	local q = queue.new()
	local idle = true
	local function add(fn)
		if idle then
			idle = false
			fn()
			idle = true
			local e = q.pop()
			while (e)
			do
				idle = false
				e.f()
				idle = true
				e = q.pop()
			end
		else
			q.push({f=fn})
		end
	end
	return add
end


return newQueue
