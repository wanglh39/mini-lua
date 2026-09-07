-- === upvalue 引用语义测试 ===

-- 1. 多个闭包共享同一 upvalue（用 table 返回多个闭包）
function make_pair()
    local x = 0
    local function get() return x end
    local function set(v) x = v end
    return {get = get, set = set}
end

local pair = make_pair()
print(pair.get())     -- 0
pair.set(42)
print(pair.get())     -- 42  (引用语义：set 修改了 x，get 能看到)

-- 2. 闭包修改 upvalue，外层也能看到（通过返回的闭包间接验证）
function accumulator()
    local sum = 0
    return function(n)
        sum = sum + n
        return sum
    end
end

local acc = accumulator()
print(acc(10))   -- 10
print(acc(20))   -- 30
print(acc(5))    -- 35

-- 3. 每次调用创建独立闭包（各自独立的 upvalue）
local a1 = accumulator()
local a2 = accumulator()
print(a1(100))   -- 100
print(a2(200))   -- 200  (a2 独立于 a1)
print(a1(1))     -- 101  (a1 不受 a2 影响)

-- 4. 嵌套闭包（隔层捕获）
function nested_closure()
    local x = 1
    local function middle()
        local y = 2
        local function inner()
            return x + y  -- x 隔层捕获，y 直接捕获
        end
        return inner
    end
    return middle
end

local m = nested_closure()
local i = m()
print(i())        -- 3  (x=1 + y=2)

-- 5. 闭包计数器（经典 upvalue 引用测试）
function counter()
    local n = 0
    return function()
        n = n + 1
        return n
    end
end

local c = counter()
print(c())   -- 1
print(c())   -- 2
print(c())   -- 3
