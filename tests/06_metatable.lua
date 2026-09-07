-- === metatable: __index 继承 ===

-- 用 table 做 prototype，通过 __index 实现继承
local Animal = {}
Animal.speak = function(self)
    return "some sound"
end

-- 创建实例，继承自 Animal
local dog = {}
setmetatable(dog, {__index = Animal})
dog.speak = function(self)
    return "Woof!"
end

local cat = {}
setmetatable(cat, {__index = Animal})

print(dog.speak(dog))    -- Woof!
print(cat.speak(cat))    -- some sound (从 Animal 继承)

-- === metatable: __index 为 function ===
local proxy = {}
setmetatable(proxy, {__index = function(t, k)
    return "key: " .. k
end})

print(proxy.hello)       -- key: hello
print(proxy.world)       -- key: world
print(proxy[42])         -- key: 42  (注意：数字键也走 __index)

-- === metatable: __newindex 代理 ===
local log = {}
local readonly = {}
setmetatable(readonly, {__newindex = function(t, k, v)
    log[#log + 1] = k .. "=" .. v
end})

readonly.x = 10
readonly.y = 20
print(log[1])            -- x=10
print(log[2])            -- y=20
print(readonly.x)       -- nil (值被代理到 log，不在 readonly 上)

-- === metatable: __index 链式继承 ===
local Base = {}
Base.method = function(self) return "base method" end

local Derived = {}
setmetatable(Derived, {__index = Base})

local obj = {}
setmetatable(obj, {__index = Derived})

print(obj.method(obj))   -- base method (obj → Derived → Base)

-- === table.insert / table.remove ===
local arr = {1, 2, 3}
table.insert(arr, 4)
print(arr[4])            -- 4
table.insert(arr, 1, 0)  -- 在位置 1 插入 0
print(arr[1])            -- 0
print(arr[2])            -- 1
print(#arr)              -- 5

local removed = table.remove(arr)  -- 移除末尾
print(removed)           -- 4
print(#arr)              -- 4

-- === table.concat ===
local parts = {"hello", "world", "lua"}
print(table.concat(parts, ", "))  -- hello, world, lua
print(table.concat(parts))        -- helloworldlua

-- === table.sort ===
local nums = {5, 3, 8, 1, 9, 2}
table.sort(nums)
print(nums[1], nums[2], nums[3], nums[4], nums[5], nums[6])  -- 1 2 3 5 8 9

local strs = {"banana", "apple", "cherry"}
table.sort(strs)
print(strs[1], strs[2], strs[3])  -- apple banana cherry