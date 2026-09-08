-- === GC 测试 ===

local function create_garbage()
    for i = 1, 10000 do
        local t = {x = i, y = i + 1, z = i + 2}
    end
end

create_garbage()
print("GC test 1 passed")

local function make_closure()
    local x = 42
    return function() return x end
end

for i = 1, 10000 do
    local f = make_closure()
    f()
end

print("GC test 2 passed")

local keep = {}
for i = 1, 100 do
    keep[i] = {value = i}
end

for i = 1, 10000 do
    local junk = {x = i}
end

print(keep[1].value)
print(keep[50].value)
print(keep[100].value)