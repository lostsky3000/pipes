

local queue = {}

function queue.new()
	local q = {sz=0}
	q.push = function(e)
		local sz = q.sz
		if sz > 0 then
			q._tail._nxt = e
			q._tail = e
		else  -- 1st item
			q._head = e
			q._tail = e 
		end
		q.sz = sz + 1
		return sz + 1
	end
	q.pop = function()
		local sz = q.sz - 1
		if sz < 0 then  -- no item
			return nil
		end
		local e = q._head
		if sz > 0 then
			q._head = e._nxt
		else  -- queue is empty
			q._head = nil
			q._tail = nil
		end
		q.sz = sz
		e._nxt = nil
		return e
	end
	q.head = function()
		return q._head
	end
	q.size = function()
		return q.sz
	end
	return q
end

return queue


