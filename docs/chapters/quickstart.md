# 快速上手

## 编译

需要 gcc 和 make。

```bash
# 编译阶段 4（= src/）
make

# 或手动编译指定阶段
gcc -std=gnu99 -Wall -Wextra -g -O2 -fno-strict-aliasing \
    stage5/*.c -o stage5/mini-lua.exe -lm
```

### 编译选项说明

| 选项 | 作用 |
|------|------|
| `-std=gnu99` | C99 + GNU 扩展（strdup 等 POSIX 函数） |
| `-Wall -Wextra` | 开启所有警告 |
| `-g` | 调试信息 |
| `-O2` | 优化级别 2 |
| `-fno-strict-aliasing` | 禁用严格别名优化（tagged union 在 O2 下 UB） |

### `-fno-strict-aliasing` 为什么必须？

我们的 `lua_Value` 是 tagged union：

```c
struct lua_Value {
    LuaType type;
    union { double n; char *s; Function *fn; } v;
};
```

`-O2` 下编译器假设不同类型的指针不指向同一地址（严格别名规则）。但我们的 union 通过不同类型成员访问同一内存，触发 UB。`-fno-strict-aliasing` 告诉编译器不要做这个假设。

## 运行

```bash
# 执行脚本
./mini-lua tests/01_basic.lua

# REPL
./mini-lua

# 命令行代码
./mini-lua -e "print('hello')"
```

## 运行测试

```bash
# 全部测试
make test

# 或手动
for f in tests/*.lua; do
    echo "  $f"
    ./mini-lua "$f" || exit 1
done
```

## 测试用例

| 测试 | 覆盖 | 阶段 |
|------|------|------|
| 01_basic.lua | 算术、字符串、布尔、nil | 1+ |
| 02_control.lua | if/while/for/break | 1+ |
| 03_function.lua | 函数、递归、闭包、高阶函数 | 1+ |
| 04_table.lua | table 构造、索引、遍历 | 3+ |
| 05_upvalue.lua | upvalue 闭包、共享变量 | 3+ |
| 06_metatable.lua | metatable、__index、OOP | 3+ |
| 07_coroutine.lua | yield/resume、状态、嵌套 | 5 |
| 08_gc.lua | 大量分配、保留对象 | 5 |

## 项目结构

```
mini-lua/
├── src/           # 阶段 4（寄存器式 VM）
├── stage1/        # 树遍历解释器
├── stage2/        # 栈式 VM（无 table）
├── stage3/        # 栈式 VM + table + metatable
├── stage4/        # 寄存器式 VM
├── stage5/        # 迭代式 VM + GC + 协程
├── tests/         # 测试用例
├── docs/          # VitePress 文档站
└── Makefile
```

## 各阶段文件对照

| 文件 | 官方对照 | 职责 |
|------|---------|------|
| llex.h/c | llex.c | 词法分析 |
| lparser.h/c | lparser.c | 语法分析（递归下降） |
| lcode.h/c | lcode.c | 编译器（AST → 字节码） |
| lopcodes.h | lopcodes.h | 指令集定义 |
| lobject.h/c | lobject.h/c | 对象系统（TValue/Table/Function/Proto） |
| lvm.h/c | lvm.c | VM 主循环 |
| ltable.h/c | ltable.c | Table 实现（哈希表） |
| ltm.h/c | ltm.c | 元方法 |
| lgc.h/c | lgc.c | 垃圾回收 |
| lcorolib.c | lcorolib.c | 协程库 |
| lbaselib.c | lbaselib.c | 基础库（print/type/...） |
| ltablib.c | ltablib.c | table 库（insert/remove/...） |
| lauxlib.h/c | lauxlib.c | 辅助库 |
| linit.c | linit.c | 库初始化 |
| main.c | lua.c | 入口 |

## 从源码阅读

建议阅读顺序（由浅入深）：

### 第一步：理解对象系统

```c
// src/lobject.h
typedef struct {
    LuaType type;
    union {
        double n;        // 数字
        char *s;         // 字符串
        Table *t;        // 表
        Closure *fn;     // 闭包
    } v;
} lua_Value;
```

所有值统一为 `lua_Value`，type 标记当前类型。这是 Lua 的核心设计——一切皆值。

### 第二步：理解词法分析

```c
// src/llex.c
Token next_token();  // 跳过空白，返回下一个 token
```

词法分析把源码字符串切分为 token 流：数字、字符串、标识符、关键字、符号。

### 第三步：理解语法分析

```c
// src/lparser.c
Proto *parse_function();  // 递归下降，生成 Proto
```

语法分析用递归下降法，每个语法规则对应一个函数：`parse_expr()`、`parse_stmt()` 等。

### 第四步：理解 VM 主循环

```c
// src/lvm.c
while (1) {
    Instruction *ins = &frame->proto->code[pc++];
    switch (ins->opcode) {
        case OP_LOADK:  ...
        case OP_ADD:    ...
        case OP_CALL:   ...
        case OP_RETURN: ...
        ...
    }
}
```

VM 是一个巨大的 switch 循环，每条指令一个 case。这是最经典的字节码 VM 结构。

### 第五步：理解 GC

```c
// src/lgc.c
void gc_collect(lua_State *L) {
    mark_roots(L);      // 从根集出发标记
    sweep(L);           // 清除未标记对象
}
```

mark-sweep 两阶段：先从根集（栈、全局表）出发标记所有可达对象，再扫描堆清除未标记的。

## 调试技巧

### 打印字节码

在编译后插入 `proto_dump(proto)` 打印所有指令：

```
function fib(n):
  0: LOADK R1 0        -- 加载常量 0
  1: LT R1 R0 R1       -- n < 0?
  2: JMP 5             -- 跳到 return
  3: SUB R2 R0 R1      -- n - 1
  4: CALL R2 1 1       -- fib(n-1)
  ...
```

### 跟踪执行

在 VM 主循环中打印每条指令：

```c
printf("%04d: %s\n", pc, op_name(ins->opcode));
```

能看到实际执行路径，理解控制流。

### GC 可视化

在 GC 前后打印对象数量：

```c
printf("GC: before=%d, after=%d\n", count_objects(L), count_after);
```

验证 GC 是否正确回收。

## 常见问题

### 编译错误：`uintptr_t` 未定义

```c
#include <stdint.h>  // 添加这个头文件
```

### 编译错误：`strdup` 未定义

用 `-std=gnu99` 而不是 `-std=c99`，或自己实现：

```c
char *my_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    return p ? memcpy(p, s, n) : NULL;
}
```

### 运行崩溃：tagged union UB

确保加了 `-fno-strict-aliasing`。O2 优化下不加会随机崩溃。

### 协程崩溃：setjmp/longjmp

我们的协程用迭代式 VM，不用 setjmp。如果看到 setjmp 相关崩溃，检查是否用了旧代码。