-- basics
print("hello", 1, -2, nil, true, false)
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print("fib", fib(20))
local t = {}
for i = 1, 10 do t[i] = i * i end
print(#t, t[3], t[10])
local s = 0
for i, v in ipairs(t) do s = s + v end
print("sum", s)
local counts = {}
for _, w in ipairs({"a", "b", "a", "c", "b", "a"}) do counts[w] = (counts[w] or 0) + 1 end
print(counts.a, counts.b, counts.c, counts.d)
-- closures
local function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c1, c2 = counter(), counter()
print(c1(), c1(), c2(), c1())
-- varargs and multiple results
local function mr() return 1, 2, 3 end
local function count(...) return select('#', ...) end
print(mr())
print(count(mr()), count(mr(), 10), count())
local a, b, c, d = mr()
print(a, b, c, d)
print(({mr()})[3], #{mr(), mr()})
-- strings
local str = "Hello" .. ", " .. "world" .. 42
print(str, #str, str:upper(), string.sub(str, 1, 5), str:sub(-2))
print(string.format("%5d|%-5s|%05d|%x|%s", 42, "ab", -42, 255, "z"))
print(("x"):rep(3, "-"), string.byte("A"), string.char(72, 105))
print(table.concat({1, 2, 3}, ", "))
-- while, repeat, break
local i = 0
while true do i = i + 1; if i >= 5 then break end end
repeat i = i - 1 until i <= 2
print("i", i)
-- integer ops
print(7 // 2, -7 // 2, 7 % 3, -7 % 3, 2 << 3, 5 & 3, 5 | 3, 5 ~ 3, ~0)
print(1 == 1, 1 ~= 1, "a" < "b", 3 <= 2, not nil)
-- tables, metatables
local Point = {}
Point.__index = Point
function Point.new(x, y) return setmetatable({x = x, y = y}, Point) end
function Point:sum() return self.x + self.y end
local p = Point.new(3, 4)
print(p:sum(), getmetatable(p) == Point)
local keys = {}
for k in pairs({x = 1, y = 2, z = 3}) do keys[#keys + 1] = k end
table.sort(keys)
print(table.concat(keys, " "))
local arr = {5, 2, 8, 1}
table.sort(arr, function(x, y) return x > y end)
print(table.concat(arr, ","))
table.insert(arr, 1, 0); table.insert(arr, 9)
print(table.concat(arr, ","), table.remove(arr), table.remove(arr, 1), #arr)
print(tostring(nil), tonumber("42") + 1, tonumber("z", 36), type({}), type(print))
print(math.max(3, 9, 2), math.min(3, 9, 2), math.abs(-5))
goto_free = "done"
print(goto_free)
