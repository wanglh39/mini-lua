-- === table 基本操作 ===
local t = {}
t[1] = 10
t[2] = 20
t["name"] = "lua"
print(t[1])        -- 10
print(t[2])        -- 20
print(t.name)      -- lua (成员访问语法)
print(t["name"])   -- lua (索引访问语法)

-- === table 构造器 ===
local a = {1, 2, 3}           -- array 形式
print(a[1], a[2], a[3])       -- 1  2  3

local p = {x = 100, y = 200}  -- record 形式
print(p.x, p.y)               -- 100  200

local mixed = {10, 20, name = "test", [100] = "special"}
print(mixed[1])               -- 10
print(mixed[2])               -- 20
print(mixed.name)             -- test
print(mixed[100])             -- special

-- === # 长度运算 ===
local arr = {10, 20, 30, 40, 50}
print(#arr)                   -- 5

-- === 嵌套 table ===
local nested = {
    inner = {1, 2, 3},
    data = {key = "value"}
}
print(nested.inner[2])        -- 2
print(nested.data.key)        -- value

-- === table 作为对象（方法调度） ===
local counter = {
    count = 0,
    inc = function(self)
        self.count = self.count + 1
        return self.count
    end
}
print(counter.inc(counter))   -- 1
print(counter.inc(counter))   -- 2
print(counter.inc(counter))   -- 3