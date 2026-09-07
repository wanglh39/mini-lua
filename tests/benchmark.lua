-- benchmark.lua: 性能基准测试

-- 1. 递归 fib(30)
function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
print("fib(30) =", fib(30))

-- 2. 累加 1..1000000
local sum = 0
for i = 1, 1000000 do
    sum = sum + i
end
print("sum 1..1000000 =", sum)

-- 3. table 操作
local t = {}
for i = 1, 100000 do
    t[i] = i * i
end
local s = 0
for i = 1, #t do
    s = s + t[i]
end
print("table sum =", s)