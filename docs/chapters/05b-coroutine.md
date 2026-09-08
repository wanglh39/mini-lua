# 阶段 5b：协程 coroutine

> 源码目录：`stage5/lcorolib.h`、`stage5/lcorolib.c`、`stage5/lvm.c`（迭代式 VM）
> 官方对照：lua-5.1.5/src/lcorolib.c、ldo.c
> 测试：07_coroutine.lua 通过

## 目标

实现协作式线程：多个执行流共享一个 C 线程，手动切换。

## 协程结构

```c
struct lua_Thread {
    GCObject gc;
    lua_State *L;     // 独立的 VM 状态（帧栈/寄存器）
    Function *fn;     // 入口函数
    CoStatus status;  // SUSPENDED / RUNNING / DEAD / NORMAL
    int first_time;   // 1=尚未启动
    lua_Value yield_value;
    lua_Value resume_value;
    lua_Thread *caller;
};
```

每个协程有独立的 `lua_State`（独立的帧栈和寄存器），但共享全局环境。

## 迭代式 VM：yield flag 机制

### 为什么不能用 setjmp/longjmp？

最初尝试 setjmp/longjmp 方案，崩溃了。根因：

1. `call_function` 是递归的，局部变量（R, p, frame）在 C 栈上
2. `longjmp` 跳回 resume 时，C 栈帧被丢弃
3. 再次 `longjmp` 回协程时，C 栈已被主程序覆盖 → 访问垃圾值 → 崩溃

**setjmp/longjmp 不保存栈内容，只保存寄存器。** 要保存栈需要 ucontext/Fiber，但那是平台相关。

### 迭代式 VM

解决方案：把 `call_function` 从递归改为迭代式（这是 Lua 官方的做法）。

```
递归式：OP_CALL → R[A] = call_function(L, fn, args)  // C 递归
迭代式：OP_CALL → 压新帧到 L->frames，继续循环       // 不递归
```

VM 状态全在堆上（`L->frames`），C 栈不随调用深度增长。

### yield/resume 流程

```
coroutine.yield(val)
  → 设置 L->yield_requested=1, L->yield_value=val
  → 返回（builtin 返回）
  → vm_execute 检测到 yield_requested
  → 保存 L->yield_reg = A（当前寄存器槽）
  → 返回 yield_value，帧栈原样保留

coroutine.resume(co, val2)
  → resume_function(co->L, val2)
  → 把 val2 写入帧顶的 regs[yield_reg]
  → 继续 vm_execute 主循环
```

关键：yield 时**不弹帧**，帧栈原样保留。resume 时从断点继续执行。

## 支持嵌套 yield

因为 VM 是迭代式的，整个调用栈都在 `L->frames` 数组里。即使 `f()` 调用 `g()` 调用 `coroutine.yield()`，yield 时帧栈 `[f, g, yield]` 全部保留，resume 时从 `yield` 点继续，`g` 返回后 `f` 继续执行。

## 限制

- **元方法中不能 yield**：`table_getindex` 调用 `call_function` 执行 `__index` 元方法，如果元方法 yield，内层 `vm_execute` 返回 yield 值，但 `table_getindex` 不知道如何处理。这是 documented limitation。
- **无错误传播**：官方 resume 返回 `false + errmsg`，我们直接 `exit(1)`。
- **无 coroutine.wrap**。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| 独立栈 | 每协程独立 lua_State | 一致 |
| yield/resume | luaD_call/luaD_resume | yield flag + resume_function |
| 嵌套 yield | 支持 | 支持 |
| 元方法 yield | 支持 | 不支持 |
| 错误传播 | 有 | 无 |