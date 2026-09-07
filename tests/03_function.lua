-- 03_function.lua: 函数、递归、闭包
print("=== 函数声明 ===")
function add(a, b)
    return a + b
end
print(add(3, 4))

print("=== 递归: fib ===")
function fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end
print("fib(10) =", fib(10))
print("fib(20) =", fib(20))

print("=== 闭包: counter ===")
function make_counter()
    local count = 0
    return function()
        count = count + 1
        return count
    end
end

local c = make_counter()
print(c())
print(c())
print(c())

print("=== 局部函数 ===")
local function square(n)
    return n * n
end
print(square(7))

print("=== 高阶函数 ===")
function apply(f, x)
    return f(x)
end
print(apply(square, 5))