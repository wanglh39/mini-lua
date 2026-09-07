# 设计哲学

Lua 的每个设计决策背后都有明确的取舍。本系列逐个解读"为什么 Lua 这么设计"。

## 已规划篇目

- [为什么只有 table](./why-only-table) — 统一数据结构的极简主义
- [为什么寄存器 VM](./why-register-vm) — 栈式 vs 寄存器式的取舍
- [为什么 1-based 索引](./why-1-based) — 历史与数学惯例的权衡
- [增量 GC 与写屏障](./incremental-gc) — 为什么不用分代 GC
- [非对称协程](./why-asymmetric-coroutine) — coroutine 的设计选择

> 篇目随实现进度逐步填充。