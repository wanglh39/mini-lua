# 阶段 5b：协程 coroutine

> 源码目录：`stage5/lcorolib.h`、`stage5/lcorolib.c`、`stage5/lvm.c`（迭代式 VM）
> 官方对照：lua-5.1.5/src/lcorolib.c、ldo.c
> 测试：07_coroutine.lua 通过

## 目标

实现协作式线程：多个执行流共享一个 C 线程，手动切换。

协程是 Lua 最独特的特性之一。与线程的区别：协程是协作式的（手动 yield 切换），不是抢占式的（时钟中断切换）。好处是无需锁——同一时刻只有一个协程在执行。

## 协程结构

```c
typedef enum {
    CO_SUSPENDED = 0,  // 已创建或已 yield，等待 resume
    CO_RUNNING,        // 正在执行
    CO_DEAD,           // 已正常结束或出错
    CO_NORMAL,         // 已 resume 别的协程（被动的）
} CoStatus;

struct lua_Thread {
    GCObject gc;          // GC 头
    lua_State *L;         // 独立的 VM 状态（帧栈/寄存器）
    Function *fn;         // 入口函数
    CoStatus status;      // 协程状态
    int first_time;       // 1=尚未启动, 0=已启动
    lua_Value yield_value;  // yield 传给 resume 的值
    lua_Value resume_value; // resume 传给 yield 的值
    lua_Thread *caller;   // 谁 resume 了我
};
```

每个协程有独立的 `lua_State`（独立的帧栈和寄存器），但共享全局环境。

## setjmp/longjmp 方案的失败

### 最初尝试

最初用 setjmp/longjmp 实现 yield/resume：

```c
// resume
if (setjmp(co->caller_jmp) == 0) {
    call_function(co->L, co->fn, args, nargs);  // 执行协程
} else {
    return co->yield_value;  // 从 yield 返回
}

// yield
if (setjmp(co->co_jmp) == 0) {
    longjmp(co->caller_jmp, 1);  // 跳回 resume
} else {
    return co->resume_value;     // 被 resume 唤醒
}
```

### 崩溃原因

测试时访问冲突（exit code -1073741784）。根因分析：

1. `call_function` 是**递归**的，局部变量（R, p, frame）在 **C 栈**上
2. `longjmp(caller_jmp)` 跳回 resume 时，C 栈帧被展开丢弃
3. 主程序继续执行，C 栈被其他函数调用覆盖
4. 下次 `longjmp(co_jmp)` 跳回 yield 点，恢复的是 setjmp 时的寄存器状态
5. 但 C 栈上 `call_function` 的局部变量已被覆盖为垃圾 → 访问冲突

**核心问题**：setjmp/longjmp 不保存栈内容，只保存寄存器。递归式 VM 的状态在 C 栈上，longjmp 回来后 C 栈是垃圾。

### 为什么有些解释器用 setjmp/longjmp 实现协程？

因为它们的 VM 是**迭代式**的！VM 的所有状态都在堆上（帧栈、寄存器），C 栈上没有重要的局部变量。longjmp 只用于在 builtin 和 VM 主循环之间跳转。

**结论**：setjmp/longjmp 实现协程的前提就是 VM 必须是迭代式的。

## 迭代式 VM 改造

### 递归式 → 迭代式

```
递归式：OP_CALL → R[A] = call_function(L, fn, args)  // C 递归
迭代式：OP_CALL → 压新帧到 L->frames，继续循环       // 不递归
```

### CallFrame 加 caller_reg

```c
typedef struct CallFrame {
    Function *fn;
    lua_Value *regs;
    int ip;
    int caller_reg;  // 父帧中存放返回值的寄存器（-1=底层帧）
} CallFrame;
```

### vm_execute 主循环

```c
static lua_Value vm_execute(lua_State *L) {
    lua_Value retval = make_nil();
    while (L->frame_count > 0) {
        CallFrame *frame = &L->frames[L->frame_count - 1];
        Function *fn = frame->fn;
        Proto *p = fn->u.user.proto;
        lua_Value *R = frame->regs;

        Instruction inst = p->code[frame->ip++];
        switch (inst.op) {
            case OP_CALL: {
                if (fn_val.v.fn->is_builtin) {
                    R[A] = fn_val.v.fn->u.builtin(&R[A+1], B);
                    if (L->yield_requested) {
                        L->yield_reg = A;
                        return L->yield_value;  // 不弹帧！
                    }
                } else {
                    // 压新帧，不递归
                    CallFrame *new_frame = &L->frames[L->frame_count++];
                    new_frame->caller_reg = A;
                    // 绑定参数...
                }
                break;
            }
            case OP_RETURN:
                retval = R[A];
                // 弹帧，回写父帧寄存器
                close_upvals(L, frame->regs, nregs);
                int caller_reg = frame->caller_reg;
                free(frame->regs);
                L->frame_count--;
                if (caller_reg >= 0)
                    L->frames[L->frame_count-1].regs[caller_reg] = retval;
                else
                    return retval;  // 底层帧
                break;
        }
    }
}
```

### call_function / resume_function

```c
lua_Value call_function(lua_State *L, Function *fn, lua_Value *args, int nargs) {
    if (fn->is_builtin) return fn->u.builtin(args, nargs);
    // 压初始帧（caller_reg = -1）
    push_frame(L, fn, args, nargs, -1);
    return vm_execute(L);
}

lua_Value resume_function(lua_State *L, lua_Value resume_value) {
    // yield 时帧栈保留，把 resume_value 写入 yield_reg 槽
    CallFrame *frame = &L->frames[L->frame_count - 1];
    frame->regs[L->yield_reg] = resume_value;
    return vm_execute(L);  // 继续执行
}
```

## yield flag 机制

### coroutine.yield

```c
static lua_Value builtin_coroutine_yield(lua_Value *args, int nargs) {
    lua_Thread *co = g_current_thread;
    co->L->yield_requested = 1;
    co->L->yield_value = (nargs >= 1) ? args[0] : make_nil();
    return make_nil();  // 返回值无关紧要——vm_execute 返回 yield_value
}
```

### coroutine.resume

```c
static lua_Value builtin_coroutine_resume(lua_Value *args, int nargs) {
    lua_Thread *co = args[0].v.th;
    g_current_thread = co;

    lua_Value result;
    if (co->first_time) {
        co->first_time = 0;
        result = call_function(co->L, co->fn, &args[1], nargs - 1);
    } else {
        result = resume_function(co->L, args[1]);  // 从 yield 继续
    }

    g_current_thread = caller;
    if (co->L->yield_requested) {
        co->L->yield_requested = 0;
        co->status = CO_SUSPENDED;
        return co->L->yield_value;  // yield 了
    } else {
        co->status = CO_DEAD;
        return result;  // 正常返回
    }
}
```

### 完整流程

```
coroutine.resume(co, 10)
  → call_function(co->L, co->fn, [10])
  → vm_execute(co->L)
  → 执行协程代码...
  → 遇到 coroutine.yield(11)
    → 设置 yield_requested=1, yield_value=11
    → vm_execute 检测到，保存 yield_reg，返回 11
  → resume 检测到 yield_requested，状态=SUSPENDED，返回 11

coroutine.resume(co, 20)
  → resume_function(co->L, 20)
  → 把 20 写入 yield_reg 槽（yield 表达式的返回值）
  → vm_execute(co->L) 继续执行
  → 遇到 coroutine.yield(21)
    → 同上，返回 21
  → ...
```

## 嵌套 yield

因为 VM 是迭代式的，整个调用栈都在 `L->frames` 数组里。即使 `f()` 调用 `g()` 调用 `coroutine.yield()`：

```
帧栈: [f的帧, g的帧, yield调用点]
```

yield 时帧栈原样保留。resume 时从 yield 点继续，`g` 返回后 `f` 继续执行。这是迭代式 VM 的天然优势——不需要额外机制处理嵌套 yield。

## 限制

### 元方法中不能 yield

`table_getindex` 调用 `call_function` 执行 `__index` 元方法。如果元方法 yield，内层 `vm_execute` 返回 yield 值，但 `table_getindex` 不知道如何处理——它把 yield 值当作元方法的返回值。

这是 documented limitation。官方 Lua 通过让所有调用都走帧栈（包括元方法调用）来支持元方法 yield。

### 无错误传播

官方 `coroutine.resume` 返回 `false, errmsg` 表示出错。我们直接 `exit(1)`。

### 无 coroutine.wrap

`coroutine.wrap(f)` 创建协程并返回一个函数，调用该函数等于 resume。省略简化。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| 独立栈 | 每协程独立 lua_State | 一致 |
| yield/resume | luaD_call/luaD_resume | yield flag + resume_function |
| 嵌套 yield | 支持 | 支持 |
| 元方法 yield | 支持 | 不支持 |
| 错误传播 | resume 返回 false+错误 | 无 |
| coroutine.wrap | 有 | 无 |

## 测试

```lua
-- 07_coroutine.lua
local co = coroutine.create(function(a)
    print("first:", a)       -- first: 10
    local b = coroutine.yield(a + 1)
    print("second:", b)      -- second: 20
    local c = coroutine.yield(b + 1)
    print("third:", c)       -- third: 30
    return c + 1
end)

print(coroutine.resume(co, 10))  -- 11
print(coroutine.resume(co, 20))  -- 21
print(coroutine.resume(co, 30))  -- 31
print(coroutine.status(co))      -- dead
```

输出完全正确。yield/resume 交替执行，参数传递正确，状态转换正确。
