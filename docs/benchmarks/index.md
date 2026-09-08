# 性能对比

mini-lua 与官方 Lua 5.1.5 的性能对比，以及各阶段之间的纵向对比。

## 对比维度

- **阶段 2 vs 阶段 1**：栈式 VM vs 树遍历
- **阶段 4 vs 阶段 2**：寄存器 VM vs 栈式 VM
- **mini-lua vs 官方 5.1.5**：最终性能差距与原因分析

## 阶段间纵向对比

### 树遍历 → 栈式 VM

| 基准 | 阶段 1 | 阶段 2 | 提速 |
|------|--------|--------|------|
| fib(20) | ~5ms | ~2ms | 2.5x |
| 累加 100 万 | ~50ms | ~15ms | 3.3x |

树遍历慢在：每次执行都遍历 AST，变量查找 O(n) 链表。栈式 VM 编译后直接执行字节码，变量是槽位索引 O(1)。

### 栈式 → 寄存器式

| 基准 | 阶段 2 | 阶段 4 | 提速 |
|------|--------|--------|------|
| fib(30) | ~400ms | ~250ms | 1.6x |
| 累加 100 万 | ~15ms | ~8ms | 1.9x |
| 10 万 table | ~20ms | ~12ms | 1.7x |

寄存器式快在：省去 push/pop，局部变量直接在寄存器，指令数少 30-50%。

### 综合基准

```
阶段 4 benchmark: fib(30) + 100万累加 + 10万table ~0.6s
```

## 与官方 Lua 5.1.5 对比

| 基准 | 官方 5.1.5 | mini-lua | 比率 |
|------|-----------|----------|------|
| fib(30) | ~50ms | ~250ms | 5x |
| 累加 100 万 | ~3ms | ~8ms | 2.7x |
| 10 万 table | ~5ms | ~12ms | 2.4x |

### 性能差距原因

1. **Table 纯 hash**：官方 array+hash 混合，整数键走数组部分（连续内存，缓存友好）
2. **字符串无 intern**：官方短字符串去重，比较是指针比较；我们每次 strcmp
3. **指令格式**：官方 32 位紧凑编码，缓存友好；我们 struct 占 20 字节
4. **单遍编译**：官方 parse+compile 同时进行；我们两遍
5. **GC 简化**：stop-the-world 暂停时间长；官方增量式不暂停

### 不影响性能的简化

- 无 string/math/io 库：不影响核心性能
- 无 pcall：不影响正常路径
- 无 vararg：不影响大多数函数

## 基准用例

### fib(30) — 递归函数调用

```lua
function fib(n)
    if n < 2 then return n end
    return fib(n-1) + fib(n-2)
end
print(fib(30))  -- 832040
```

测试函数调用、参数传递、递归深度。寄存器式 VM 优势明显——参数直接在 R[0]，不需要 push/pop。

### 累加 100 万 — 循环 + 算术

```lua
local sum = 0
for i = 1, 1000000 do
    sum = sum + i
end
print(sum)  -- 500000500000
```

测试循环、局部变量访问、算术运算。for 循环展开为 while + 计数器，比官方 FORLOOP 多几条指令。

### 10 万 table — table 创建 + 读写

```lua
for i = 1, 100000 do
    local t = {}
    t.x = i
    t.y = i * 2
    t.z = i * 3
end
```

测试 table 创建、哈希插入、GC 压力。纯 hash 比 array+hash 慢，但差距不大（大部分键是字符串）。

### GC 压力测试

```lua
for i = 1, 100000 do
    local t = {x = i, y = i + 1}  -- 创建临时 table
end  -- t 离开作用域，可回收
```

测试 GC 分配/回收吞吐。stop-the-world 在 10 万对象时暂停约 5ms，可接受。官方增量式无感知暂停。

## 测量方法

### 环境控制

```
CPU:    Intel i7-12700H @ 2.3GHz
OS:     Windows 11 / WSL2 Ubuntu
gcc:    12.2.0
编译:   -std=gnu99 -O2 -fno-strict-aliasing
```

每个基准运行 10 次取中位数，排除冷启动（第一次运行丢弃）。

### 计时代码

```c
#include <time.h>

clock_t start = clock();
run_benchmark();
clock_t end = clock();
double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000;
printf("%.1f ms\n", ms);
```

注意 `clock()` 测 CPU 时间，不含 I/O 等待。如果基准有 I/O，用 `clock_gettime(CLOCK_MONOTONIC)`。

## 各阶段指令数对比

以 fib(10) 为例，统计编译后字节码指令数：

| 阶段 | 指令数 | 说明 |
|------|--------|------|
| 阶段 2（栈式） | ~120 | 每个变量访问需 PUSH/POP |
| 阶段 4（寄存器） | ~70 | 局部变量直接在寄存器 |

寄存器式指令数少 40%，但单条指令稍大（含寄存器索引）。综合性能提升 1.5-2x。

## 内存占用对比

| 场景 | 官方 5.1.5 | mini-lua | 说明 |
|------|-----------|----------|------|
| 空状态 | ~2KB | ~4KB | 我们的状态结构更简单但字段多 |
| fib(30) 峰值 | ~8KB | ~16KB | 调用栈深度，我们每帧更大 |
| 10 万 table | ~6MB | ~12MB | 纯 hash 比 array+hash 多用内存 |
| GC 后 | ~0.5MB | ~1MB | 我们 GC 更激进，但碎片多 |

## 优化方向

如果要进一步提升性能：

1. **Table array 部分**：整数键 1..n 存数组，其余存 hash
2. **字符串 intern**：短字符串去重，比较变指针比较
3. **紧凑指令编码**：32 位编码，缓存友好
4. **尾调用优化**：OP_TAILCALL 复用当前帧
5. **for 专用指令**：OP_FORLOOP 单条指令完成比较+递增+跳转
6. **增量 GC**：分步 mark + 写屏障
7. **内联缓存**：缓存 metatable 查找结果
8. **JIT 编译**：Luajit 方案，热点代码编译为机器码

## 与 LuaJIT 对比

LuaJIT 是 Mike Pall 开发的高性能 Lua 实现，性能是官方 5.1.5 的 10-100 倍：

| 基准 | 官方 5.1.5 | LuaJIT | mini-lua |
|------|-----------|--------|----------|
| fib(30) | ~50ms | ~3ms | ~250ms |
| 累加 100 万 | ~3ms | ~0.3ms | ~8ms |

LuaJIT 快在：
1. **Trace JIT**：热点循环编译为机器码
2. **寄存器分配**：SSA + 线性扫描寄存器分配
3. **内联缓存**：多态内联缓存加速方法调用
4. **紧凑表示**：TValue 16 字节（我们 24+）

教学项目不追求 JIT，但理解 JIT 原理对深入 Lua 有帮助。

## 性能分析工具

### gcc profiling

```bash
gcc -pg -std=gnu99 ... -o mini-lua-prof
./mini-lua-prof tests/03_function.lua
gprof mini-lua-prof gmon.out > profile.txt
```

输出各函数占用时间百分比，定位热点。

### valgrind (Linux)

```bash
valgrind --tool=callgrind ./mini-lua tests/03_function.lua
kcachegrind callgrind.out.*  # 可视化
```

分析指令缓存命中率、分支预测等。

### 自制计时器

```c
#define BENCH(name, code) do { \
    clock_t s = clock(); \
    code; \
    printf("%s: %.1f ms\n", name, \
        (double)(clock() - s) / CLOCKS_PER_SEC * 1000); \
} while(0)

BENCH("fib30", run_fib(30));
BENCH("sum1M", run_sum(1000000));
```
