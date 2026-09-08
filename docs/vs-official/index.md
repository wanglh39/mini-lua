# 对照官方源码

本页系统对比 mini-lua 与 Lua 5.1.5 官方实现的差异。

## 对象系统

| 方面 | 官方 | 我们 |
|------|------|------|
| TValue | 有 `iscollectable()` 标记位 | 简单 tagged union |
| 字符串 | 短字符串 intern 池（去重+快速比较） | 直接 `strdup` |
| Table | **array + hash 混合**（整数键存数组部分） | **纯 hash**（开放寻址+线性探测） |
| 闭包 | LClosure + CClosure 两种 | 单一 Function（union 内置/用户） |
| Proto | LineInfo、LocVars、source 等调试信息 | 只有 local_names |

## 指令集

官方 38 条，我们 30 条。

| 缺少的指令 | 官方用途 | 我们的替代 |
|-----------|---------|-----------|
| `OP_SELF` | `obj:method()` 语法糖 | GETTABLE + CALL |
| `OP_TAILCALL` | 尾调用优化 | 不支持 |
| `OP_FORLOOP/OP_FORPREP` | 数值 for 专用 | 编译为 while + 计数器 |
| `OP_TFORLOOP/OP_TFORPREP` | 泛型 for | 编译为 while + next |
| `OP_SETLIST` | table 批量构造 | 逐条 SETTABLEK |
| `OP_VARARG` | 可变参数 `...` | 不支持 |
| `OP_TESTSET` | TEST + MOVE 合并 | TEST + MOVE |
| `OP_CLOSE` | 显式关闭 upvalue | RETURN 时统一关闭 |

多出：`OP_NE`/`OP_GT`/`OP_GE`（官方用 NOT 实现）、`OP_SETTABLEK`、`OP_TESTN`。

指令格式：官方 32 位紧凑编码（Op:6 A:8 B:9 C:9），我们用 struct — 更直观但占内存多。

## VM

| 方面 | 官方 | 我们 |
|------|------|------|
| 调用帧 | CallInfo 数组 + ci 指针 | 帧栈 + 循环（迭代式） |
| 尾调用 | OP_TAILCALL 复用当前帧 | 不支持 |
| 多返回值 | `OP_RETURN B`（B>1） | 单返回值 |
| 可变参数 | OP_VARARG | 不支持 |
| 错误处理 | setjmp/longjmp + pcall | exit(1)，无 pcall |
| 编译方式 | 单遍（parse+compile 同时） | 两遍（先 AST 再字节码） |

## GC

| 方面 | 官方 | 我们 |
|------|------|------|
| 算法 | **增量式三色** + 写屏障 | **stop-the-world 两色** |
| 执行 | 分步与 VM 交替 | 一次性 mark+sweep |
| 弱表 | `__mode = "k"/"v"` | 无 |
| finalizer | `__gc` 元方法 | 无 |
| 内存统计 | 按字节 | 按对象计数 |

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

| 库 | 官方 | 我们 |
|----|------|------|
| base | ~20 个 | 11 个 |
| string | 14 个 | **无** |
| math | 18 个 | **无** |
| io | open/read/write/close/lines | **无** |
| os | time/date/clock/execute | **无** |
| table | insert/remove/concat/sort/maxn | 4 个 |
| debug | traceback/getinfo/... | **无** |
| coroutine | create/resume/yield/status/wrap | 4 个 |

## 语法

**不支持**：`repeat-until`、`goto/label`、`...`（vararg）、多赋值、多返回值、`obj:method()` 语法糖、长字符串 `[[...]]`、十六进制 `0xFF`。

## 总结

**核心设计一致**：寄存器式 VM、upvalue open/closed 状态机、迭代式帧栈、mark-and-sweep GC、协程独立 lua_State。

**主要简化**：Table 纯 hash、GC stop-the-world、单返回值无 vararg、无 pcall、无 string/math/io/os 库、字符串无 intern、for 循环用通用指令展开。

这些简化都是教学合理的——核心概念全部保留，省去的是性能优化和工程便利设施。
