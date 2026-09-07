# 实现路线图

对标 Lua 5.1.5 子集，分 5 个阶段递进。每阶段需产出可演示成果并同步文档。

---

## 阶段 1：树遍历解释器

**目标**：Lua 极小子集——`print` / 算术 / `local` / `if` / `while` / `for` / 函数调用。能跑 `print(fib(10))`。

**不对照官方源码**，自行设计 AST 与求值器，建立信心。

**产出**：
- 可执行 `mini-lua`
- `tests/01_basic.lua` 等 10 个测试用例
- `docs/chapters/01-treewalk.md` 实现笔记

**预计周期**：2 周

---

## 阶段 2：栈式字节码 VM

**目标**：把阶段 1 拆成"编译器 + VM"。引入 `lua_State`、`TValue`、`CallInfo` 雏形。

**对照官方**：`lopcodes.h`、`lvm.c` 的 `luaV_execute` 主循环、`lcode.c` 指令发射。

**产出**：
- 相同测试用例通过（走字节码路径）
- `luac -l` 风格的反汇编工具
- `docs/chapters/02-stack-vm.md`

**难点**：
- 指令集设计
- 跳转指令回填
- `TValue` 的 tagged union

**预计周期**：3–4 周

---

## 阶段 3：闭包 + table + metatable

**目标**：完整 `table`（数组+哈希合一）、`local function` 递归、upvalue、`__index`/`__newindex`/运算符元方法。能写 OOP 风格代码。

**对照官方**：`ltable.c`（`luaH_get`/`luaH_set`、数组与哈希边界）、`lfunc.c`（`UpVal` open/closed 状态机）、`ltm.c`。

**产出**：
- 能跑官方 OOP 示例
- `docs/chapters/03-closure-table.md`
- `docs/philosophy/why-only-table.md`

**难点**：
- **upvalue open/closed 转换**：官方用链表管理 open upvalue，理解这张状态图是关键
- **table rehash 与数组/哈希分区**：`luaH_resize` 的分配策略

**预计周期**：5–6 周

---

## 阶段 4：寄存器式 VM

**目标**：重写为寄存器式，指令集对齐 `lopcodes.h`（`OP_MOVE`/`OP_LOADK`/`OP_GETUPVAL`/`OP_CALL` 等）。

**对照官方**：[The Implementation of Lua 5.0](https://www.lua.org/doc/josl05.pdf) 论文 **必读**，`lcode.c` 寄存器分配。

**产出**：
- 性能对比阶段 2，应有 2–4 倍提升
- `docs/chapters/04-register-vm.md`
- `docs/benchmarks/stack-vs-register.md`

**难点**：
- 寄存器分配
- 表达式分类（`EXP_VAREG`/`EXP_NONRELOC`/`EXP_CALL`）决定寄存器释放

**预计周期**：4–5 周

---

## 阶段 5a：增量标记-清除 GC

**目标**：三色标记 + 增量步进。大循环不爆内存。

**对照官方**：`lgc.c`。5.1 的 GC 是增量式（非分代），核心是 `luaC_step` 步进控制与 `traverse`/`propagate` 队列。

**产出**：
- 压力测试 + GC 日志可视化
- `docs/chapters/05a-gc.md`
- `docs/philosophy/incremental-gc.md`

**难点**：
- **写屏障**（`luaC_barrier`）：为什么增量 GC 需要写屏障、Lua 用哪种
- **短/长字符串分离**：`lstring.c` 哈希表，GC 特例
- **open upvalue 的 GC 处理**：它是根集一部分

**预计周期**：4–5 周

---

## 阶段 5b：coroutine

**目标**：`coroutine.create`/`resume`/`yield`，非对称协程。

**对照官方**：`lstate.c` 的 `lua_newthread`、`ldo.c` 的 `luaD_call`/`luaD_precall`、`lvm.c` 的 `OP_CALL`/`OP_RETURN` 处理 yield 跨栈。

**产出**：
- 协程测试用例
- `docs/chapters/05b-coroutine.md`
- `docs/philosophy/why-asymmetric-coroutine.md`

**难点**：
- 协程本质是独立栈的函数调用
- `lua_State` 栈切换
- yield 时挂起中间帧
- 对照：5.1 用独立栈，5.4 改共享栈，值得专门写一篇对比

**预计周期**：3 周

---

## 总周期估算

约 6–9 个月，每周稳定投入 8–10 小时。实际通常 ×1.5。

## 防弃坑策略

1. **每阶段有可演示产出**，不闷头写三个月
2. **文档与代码同步**，每模块一篇笔记
3. **以官方 5.1.5 为答案书**，卡住时对照看
4. **每周一篇 devlog**，公开推进
5. **每阶段打 tag**：`v0.1-treewalk`、`v0.2-stack-vm`…