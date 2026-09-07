-- 02_control.lua: 控制流
print("=== if/elseif/else ===")
local x = 15
if x < 10 then
    print("small")
elseif x < 20 then
    print("medium")
else
    print("large")
end

print("=== while ===")
local i = 1
local sum = 0
while i <= 10 do
    sum = sum + i
    i = i + 1
end
print("1+2+...+10 =", sum)

print("=== for ===")
for i = 1, 5 do
    print("i =", i)
end

print("=== for with step ===")
for i = 10, 1, -2 do
    print("倒序:", i)
end

print("=== break ===")
for i = 1, 100 do
    if i > 3 then
        break
    end
    print("break test:", i)
end