# 对照官方源码

本页系统对比 mini-lua 与 Lua 5.1.5 官方实现的差异——哪里偷了懒、哪里做了简化、哪里有不同选择。

## 对象系统

| 方面 | 官方 | 我们 | 简化原因 |
|------|------|------|---------|
| TValue | 有 `iscollectable()` 标记位 | 简单 tagged union | 教学简化 |
| 字符串 | 短字符串 intern 池（去重+快速比较） | 直接 `strdup` | 省去字符串池管理 |
| Table | **array + hash 混合** | **纯 hash**（开放寻址+线性探测） | 省去 rehash 分区策略 |
| 闭包 | LClosure + CClosure 两种 | 单一 Function（union） | 合并简化 |
| Proto | LineInfo、LocVars、source 等调试信息 | 只有 local_names | 省去调试信息 |

### Table 的 array+hash 混合

官方 Lua 的 Table 是最精妙的数据结构之一：

```text
// 官方
struct Table {
    TValue *array;  // 数组部分（整数键 1..n）
    Node *node;     // 哈希部分（其余键）
    int sizearray;  // 数组大小
    int lsizenode;  // 哈希大小（log2）
};
```

rehash 时计算最优分区：哪些键放数组、哪些放哈希。目标是最大化数组部分（缓存友好）。

我们纯哈希——所有键统一处理，省去分区逻辑。代价是整数键访问不如数组快。

## 指令集

官方 38 条，我们 30 条。

| 缺少的指令 | 官方用途 | 我们的替代 | 影响 |
|-----------|---------|-----------|------|
| `OP_SELF` | `obj:method()` 语法糖 | GETTABLE + CALL | 多一条指令 |
| `OP_TAILCALL` | 尾调用优化（复用当前帧） | 不支持 | 深递归可能栈溢出 |
| `OP_FORLOOP/OP_FORPREP` | 数值 for 专用指令 | while + 计数器 | 多几条指令 |
| `OP_TFORLOOP/OP_TFORPREP` | 泛型 for `for k,v in pairs(t)` | while + next | 多几条指令 |
| `OP_SETLIST` | table 批量构造 `{1,2,3}` | 逐条 SETTABLEK | 大 table 构造慢 |
| `OP_VARARG` | 可变参数 `...` | 不支持 | 无法用 `...` |
| `OP_TESTSET` | TEST + MOVE 合并 | TEST + MOVE | 多一条指令 |
| `OP_CLOSE` | 显式关闭 upvalue | RETURN 时统一关闭 | 语义一致 |

多出的指令：

| 指令 | 官方做法 | 我们的做法 |
|------|---------|-----------|
| `OP_NE` | NOT EQ | 独立指令 |
| `OP_GT` | NOT LE | 独立指令 |
| `OP_GE` | NOT LT | 独立指令 |
| `OP_SETTABLEK` | OP_SETTABLE + 常量 | 独立指令（常量键优化） |
| `OP_TESTN` | NOT TEST | 独立指令（or 短路） |

### 指令格式

| | 官方 | 我们 |
|---|------|------|
| 编码 | 32 位紧凑（Op:6 A:8 B:9 C:9） | struct {OpCode, int A/B/C, int line} |
| 大小 | 4 字节/指令 | ~20 字节/指令 |
| 优势 | 紧凑、缓存友好 | 直观、可读 |

## VM

| 方面 | 官方 | 我们 |
|------|------|------|
| 调用帧 | CallInfo 数组 + ci 指针 | 帧栈 + 循环（迭代式） |
| 尾调用 | OP_TAILCALL 复用当前帧 | 不支持 |
| 多返回值 | `OP_RETURN B`（B>1 返回多个） | 单返回值 |
| 可变参数 | OP_VARARG | 不支持 |
| 错误处理 | setjmp/longjmp + pcall | exit(1)，无 pcall |
| 编译方式 | 单遍（parse+compile 同时） | 两遍（先 AST 再字节码） |

### 单遍 vs 两遍编译

官方 Lua 在语法分析的同时生成字节码——`luaY_parser` 内部调用 `luaK_*` 发射指令。不需要 AST。

我们分两遍：先 `parse` 生成 AST，再 `compile` 遍历 AST 生成字节码。好处是解耦——语法分析和编译独立，阶段 1 可以直接遍历 AST 执行。代价是多一次遍历和 AST 的内存开销。

## GC

| 方面 | 官方 | 我们 |
|------|------|------|
| 算法 | **增量式三色**（白/灰/黑） | **stop-the-world 两色**（白/黑） |
| 执行 | 分步与 VM 交替 | 一次性 mark+sweep |
| 写屏障 | 有（luaC_barrier） | 无 |
| 弱表 | `__mode = "k"/"v"` | 无 |
| finalizer | `__gc` 元方法 | 无 |
| 内存统计 | 按字节（luaM_realloc 维护） | 按对象计数 |

### 增量 GC 为什么需要写屏障？

增量 GC 在 mark 过程中 VM 仍在运行。如果黑色对象（已标记完）新引用了白色对象（未标记），下次 mark 时不会再看黑色对象，白色对象会被错误回收。

写屏障在每次赋值时检查：黑色引用白色 → 把白色变灰，确保不会被漏掉。

我们 stop-the-world 不需要——mark 时 VM 暂停，不会有新引用。

## 协程

| 方面 | 官方 | 我们 |
|------|------|------|
| 独立栈 | 每协程独立 lua_State | 一致 |
| yield/resume | luaD_call/luaD_resume | yield flag + resume_function |
| 嵌套 yield | 支持 | 支持 |
| 元方法 yield | 支持 | 不支持 |
| 错误传播 | resume 返回 false+错误 | 无 |
| coroutine.wrap | 有 | 无 |

## 标准库

| 库 | 官方 | 我们 | 缺失 |
|----|------|------|------|
| base | ~20 个 | 11 个 | pcall, select, unpack, require, load, dofile, next, _G |
| string | 14 个 | **无** | 全部（len, sub, find, gmatch, gsub, format...） |
| math | 18 个 | **无** | 全部（sin, cos, sqrt, log, random...） |
| io | 6 个 | **无** | 全部（open, read, write, close, lines...） |
| os | 7 个 | **无** | 全部（time, date, clock, execute...） |
| table | 5 个 | 4 个 | maxn |
| debug | 10+ 个 | **无** | 全部 |
| coroutine | 5 个 | 4 个 | wrap |

## 语法

**不支持**：

| 语法 | 官方 | 我们 |
|------|------|------|
| `repeat-until` | ✓ | ✗ |
| `goto/label` | ✓ | ✗ |
| `...`（vararg） | ✓ | ✗ |
| 多赋值 `a,b=1,2` | ✓ | ✗ |
| 多返回值 | ✓ | ✗ |
| `obj:method()` | ✓ | ✗ |
| 长字符串 `[=[...]=]` | ✓ | ✗ |
| 十六进制 `0xFF` | ✓ | ✗ |

## 总结

**核心设计一致**：
- 寄存器式 VM（对齐 Lua 5.1）
- upvalue open/closed 状态机
- 迭代式帧栈（非递归 call_function）
- mark-and-sweep GC
- 协程独立 lua_State + yield/resume

**主要简化**：
1. Table 纯 hash（无 array 部分）
2. GC stop-the-world（非增量，无写屏障）
3. 单返回值无 vararg
4. 无 pcall/错误处理
5. 无 string/math/io/os 库
6. 字符串无 intern
7. for 循环用通用指令展开
8. 两遍编译（先 AST 再字节码）

这些简化都是教学合理的——核心概念全部保留，省去的是性能优化和工程便利设施。
