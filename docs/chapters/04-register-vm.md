# 阶段 4：寄存器式 VM

> 源码目录：`stage4/`（= `src/`）
> 官方对照：lua-5.1.5/src/lopcodes.h、lvm.c
> 测试：01-06 全部通过

## 目标

从栈式 VM 改为寄存器式 VM，对齐官方 Lua 5.1 的指令集设计。

## 为什么寄存器式？

| | 栈式 | 寄存器式 |
|---|------|---------|
| 指令示例 | `ADD`（弹两个压一个） | `OP_ADD A B C`（R[A]=R[B]+R[C]） |
| 指令数 | 多（需要 PUSH/POP） | 少（直接读写寄存器） |
| 栈访问 | 每条指令 2~3 次 | 减到最少 |
| 适合 | 栈式 CPU | 现代 CPU（寄存器分配友好） |

## 指令集

```c
OP_LOADK    R[A] = K[B]           // 加载常量
OP_MOVE     R[A] = R[B]           // 寄存器复制
OP_GETUPVAL R[A] = UpVal[B]       // upvalue 读
OP_GETGLOBAL R[A] = Gbl[K[B]]     // 全局变量读
OP_GETTABLE R[A] = R[B][R[C]]     // table 索引
OP_ADD      R[A] = R[B] + R[C]    // 算术
OP_CALL     R[A] = R[A](R[A+1..A+B])  // 函数调用
OP_RETURN   return R[A]           // 返回
OP_CLOSURE  R[A] = closure(Proto[B])  // 创建闭包
// ... 共 30 条指令
```

**指令格式**：官方用 32 位紧凑编码（Op:6 A:8 B:9 C:9），我们用 `struct {OpCode, int A/B/C, int line}` — 更直观但占内存多。

## 寄存器布局

```
R[0..nparams-1]        — 函数参数
R[nparams..nlocals-1]  — 局部变量
R[nlocals..nregs-1]    — 表达式求值的临时寄存器
```

`safe_reg` 机制：编译器分配临时寄存器时取 `max(当前已用+1, nlocals)`，避免覆盖局部变量。

## 关键改动

### 相对跳转

阶段 2 的 `OP_JMP target`（绝对）→ 阶段 4 的 `OP_JMP offset`（相对）。

相对跳转更适合代码移动：如果前面插入指令，不需要修正所有跳转目标。

### for 循环展开

官方有 `OP_FORLOOP`/`OP_FORPREP` 专用指令。我们编译为 `while` + 计数器（OP_ADD + OP_TEST + OP_JMP），更简单但指令更多。

## 对照官方

官方 38 条指令，我们 30 条。缺少 OP_SELF（method call）、OP_TAILCALL（尾调用）、OP_FORLOOP（for 专用）、OP_VARARG（可变参数）等。详见 [对照官方](../vs-official/)。