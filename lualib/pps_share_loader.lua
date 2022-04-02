
package.path = LUA_PATH

local tb = require(TABLE_NAME)

LUA_PATH = nil
TABLE_NAME = nil

return tb

