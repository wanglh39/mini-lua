-- === 协程测试 ===

-- 1. 基本 yield/resume
local co = coroutine.create(function(a)
    print("first:", a)       -- first: 10
    local b = coroutine.yield(a + 1)
    print("second:", b)      -- second: 20
    local c = coroutine.yield(b + 1)
    print("third:", c)       -- third: 30
    return c + 1
end)

print(coroutine.resume(co, 10))  -- 11 (yield a+1)
print(coroutine.resume(co, 20))  -- 21 (yield b+1)
print(coroutine.resume(co, 30))  -- 31 (return c+1)
print(coroutine.status(co))      -- dead

-- 2. 协程状态
local co2 = coroutine.create(function()
    coroutine.yield(1)
    coroutine.yield(2)
end)

print(coroutine.status(co2))     -- suspended
coroutine.resume(co2)
print(coroutine.status(co2))     -- suspended
coroutine.resume(co2)
print(coroutine.status(co2))     -- dead

-- 3. 生产者-消费者
local function producer()
    for i = 1, 3 do
        coroutine.yield(i * 10)
    end
end

local prod = coroutine.create(producer)
print(coroutine.resume(prod))    -- 10
print(coroutine.resume(prod))    -- 20
print(coroutine.resume(prod))    -- 30
print(coroutine.status(prod))    -- dead

-- 4. 累加器协程
local function accumulator()
    local sum = 0
    while true do
        local x = coroutine.yield(sum)
        sum = sum + x
    end
end

local acc = coroutine.create(accumulator)
coroutine.resume(acc)            -- 启动，返回 sum=0
print(coroutine.resume(acc, 10)) -- 10
print(coroutine.resume(acc, 20)) -- 30
print(coroutine.resume(acc, 5))  -- 35