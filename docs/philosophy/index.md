# 设计哲学

Lua 的每个设计决策背后都有明确的取舍。本系列逐个解读"为什么 Lua 这么设计"。

Lua 的设计优先级是：**简单性 > 效率 > 完整性**。这不是口号——每个特性都遵循这个排序。

## 为什么只有 table

Lua 只有一个数据结构：table。数组、字典、对象、命名空间全是 table。

```lua
-- 数组
local arr = {10, 20, 30}

-- 字典
local dict = {name = "Alice", age = 30}

-- 对象
local obj = {}
obj.__index = obj
function obj.new() return setmetatable({}, obj) end
function obj:method() print("hello") end
```

为什么不分出数组、字典、类？因为：
1. **实现简单**：只维护一套哈希代码
2. **语义统一**：`a[1]` 和 `a.name` 是同一操作（table 索引）
3. **互操作友好**：嵌入 C 时只需要一个 `lua_gettable` API

代价是性能——纯哈希不如连续数组快。官方 Lua 用 array+hash 混合优化，我们简化为纯哈希。

## 为什么寄存器 VM

Lua 5.0 用栈式 VM，5.1 改为寄存器式。为什么？

栈式：
```
PUSH 3    PUSH 4    PUSH 5    MUL    ADD
```
5 条指令，每条访问栈顶。

寄存器式：
```
LOADK R0 3    LOADK R1 4    LOADK R2 5    MUL R3 R1 R2    ADD R0 R0 R3
```
5 条指令，但局部变量直接在寄存器——不需要每次 PUSH/POP。

实际中局部变量占多数，寄存器式省去大量 push/pop，指令数减少 30-50%。

## 为什么 1-based 索引

Lua 的数组从 1 开始，不是 0。争议很大，但有道理：

1. **数学惯例**：数学中序列从 a₁ 开始，不是 a₀
2. **人类直觉**：非程序员说"第一个"不是"第零个"
3. **与 #t 一致**：`t[1]` 到 `t[#t]` 是全部元素，`t[#t+1]` 是追加位置

代价是 C 互操作时要 `idx - 1` 转换。

## 增量 GC 与写屏障

官方 Lua 用增量三色 GC，不是 stop-the-world。为什么？

stop-the-world 的问题：GC 时暂停整个程序，大程序暂停几百毫秒——游戏帧率掉到 0。

增量 GC：把 mark 分成小步，每步与 VM 交替执行。但引入新问题：mark 过程中程序可能修改引用（黑色对象引用了白色对象），需要**写屏障**：

```c
// 写屏障：黑色对象引用白色对象时，把白色变灰
void luaC_barrier(lua_State *L, GCObject *o, GCObject *v) {
    if (isblack(o) && iswhite(v))
        reallymarkobject(g, v);  // 把 v 变灰
}
```

我们简化为 stop-the-world——教学项目不需要实时性。

## 非对称协程

Lua 的协程是**非对称**的：`yield` 只能回到直接 `resume` 它的地方，不能跳到任意协程。

对称协程（如 Go 的 goroutine）可以任意切换。非对称协程形成调用栈——`resume` 类似函数调用，`yield` 类似 return。

好处：
1. **语义清晰**：yield/resume 就是函数调用/返回的泛化
2. **实现简单**：不需要调度器
3. **确定性**：执行顺序由代码决定，不是调度器

代价：不能直接实现生产者-消费者管道（需要手动 resume 传递控制权）。

## 为什么只用 double

Lua 5.1 的数字只有一种类型：double。没有 int、float、long。

```lua
print(type(1))       -- number
print(type(1.5))     -- number
print(1 / 3)         -- 0.33333333333333
print(2^53 + 1)      -- 9007199254740992 （精度丢失！）
```

为什么？

1. **简单性**：一种数字类型，一套算术，不需要类型转换规则
2. **安全**：不会整数溢出（`2^31 + 2^31` 在 int 溢出，在 double 不会）
3. **嵌入式友好**：Lua 定位嵌入 C，宿主用 double 就用 double

代价：
- 整数运算精度受限于 double 的 53 位尾数（2^53 以上丢失）
- 性能：double 运算比 int 慢（虽然现代 CPU 差距很小）
- 位运算：5.1 没有位运算（5.2 引入 `//` 整除和位运算库）

Lua 5.3 引入了整数子类型，但 5.1 的纯粹 double 是刻意的简化。

## 为什么不用引用计数

Python 用引用计数 + 周期性 mark-sweep 处理循环引用。Lua 直接用 mark-sweep，不用引用计数。

引用计数的问题：
1. **循环引用**：`a.next = b; b.next = a` → 引用计数永远 ≥1，需要额外 GC
2. **每次赋值都要更新计数**：`t.x = v` 要给 v 加引用、给旧值减引用，热路径开销
3. **线程不友好**：多线程下引用计数需要原子操作

mark-sweep 的优势：
1. **分配时不记账**：只管分配，GC 时统一扫描
2. **循环引用自然处理**：从根集出发可达性分析，不可达就回收
3. **批量处理**：一次扫描整个堆，比每次赋值更新高效

代价：GC 时短暂暂停（stop-the-world）。官方用增量式缓解，我们简化为 stop-the-world。

## 为什么函数是一等公民

Lua 的函数可以赋值给变量、作为参数传递、作为返回值：

```lua
local add = function(a, b) return a + b end  -- 赋值
local apply = function(f, x) return f(x) end  -- 参数
local counter = function()
    local n = 0
    return function() n = n + 1; return n end  -- 返回值
end
```

这带来：
1. **灵活**：高阶函数 map/filter/fold 自然实现
2. **闭包**：函数携带环境，状态封装不需要对象
3. **简化对象系统**：table + 闭包就能模拟 OOP，不需要 class 关键字

代价：函数不能内联（调用目标运行时确定），性能不如静态分派。

## 为什么没有 class

Lua 没有 class 关键字。OOP 用 table + metatable 模拟：

```lua
local Animal = {}
Animal.__index = Animal

function Animal.new(name)
    return setmetatable({name = name}, Animal)
end

function Animal:speak()
    return "I am " .. self.name
end

local a = Animal.new("Rex")
print(a:speak())  -- I am Rex
```

`a:speak()` 实际是 `Animal.speak(a)`——`__index` 元方法让 `a` 找到 `Animal.speak`。

为什么不用 class？
1. **table 已经够用**：对象就是 table，方法就是 table 里的函数
2. **原型继承更灵活**：metable 链可以运行时修改，class 继承是静态的
3. **少一个概念**：不需要 class、instance、method 三套语义

代价：没有类型检查，IDE 补全困难。

## 为什么不用异常

Lua 用 `error()` + `pcall()` 处理异常，不是 try/catch 语法。

```lua
local ok, err = pcall(function()
    error("something wrong")
end)
if not ok then print(err) end  -- something wrong
```

为什么不用 try/catch？
1. **setjmp/longjmp 实现**：不需要 C++ 异常机制，纯 C 实现
2. **简单**：pcall 返回 (ok, result)，不需要新语法结构
3. **显式**：错误必须显式处理，不会被意外吞掉

代价：pcall 有性能开销（setjmp），错误信息是 string 不是结构化对象。

## 已规划篇目

- [为什么只有 table](./why-only-table) — 统一数据结构的极简主义
- [为什么寄存器 VM](./why-register-vm) — 栈式 vs 寄存器式的取舍
- [为什么 1-based 索引](./why-1-based) — 历史与数学惯例的权衡
- [增量 GC 与写屏障](./incremental-gc) — 为什么不用分代 GC
- [非对称协程](./why-asymmetric-coroutine) — coroutine 的设计选择

> 篇目随实现进度逐步填充。
