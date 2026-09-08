# 阶段 4：寄存器式 VM

> 源码目录：`stage4/`（= `src/`）
> 官方对照：lua-5.1.5/src/lopcodes.h、lvm.c
> 测试：01-06 全部通过

## 目标

从栈式 VM 改为寄存器式 VM，对齐官方 Lua 5.1 的指令集设计。

这是 Lua 5.1 相对于 5.0 最重要的改进。栈式 VM 每条指令都要 push/pop，指令数多；寄存器式直接读写 R[A]/R[B]/R[C]，指令更少更高效。

## 为什么寄存器式？

| | 栈式 | 寄存器式 |
|---|------|---------|
| 指令示例 | `ADD`（弹两个压一个） | `OP_ADD A B C`（R[A]=R[B]+R[C]） |
| 指令数 | 多（需要 PUSH/POP） | 少（直接读写寄存器） |
| 栈访问 | 每条指令 2~3 次 | 减到最少 |
| 适合 | 栈式 CPU | 现代 CPU（寄存器分配友好） |

### 计算 `3 + 4 * 5` 的对比

栈式（6 条）：
```
OP_NUMBER 3    OP_NUMBER 4    OP_NUMBER 5    OP_MUL    OP_ADD
```

寄存器式（3 条）：
```
OP_LOADK R0 3    OP_LOADK R1 4    OP_LOADK R2 5
OP_MUL  R3 R1 R2    OP_ADD  R0 R0 R3
```

虽然寄存器式多了几条 LOADK，但省去了 push/pop 的间接访问。实际中局部变量直接在寄存器里，不需要每次加载。

## 指令集

```c
typedef enum {
    /* 值加载 */
    OP_LOADK,      // R[A] = K[B]
    OP_LOADNIL,    // R[A] = nil
    OP_LOADBOOL,   // R[A] = (B != 0)

    /* 寄存器复制 */
    OP_MOVE,       // R[A] = R[B]

    /* upvalue */
    OP_GETUPVAL,   // R[A] = UpVal[B]
    OP_SETUPVAL,   // UpVal[B] = R[A]

    /* 全局变量 */
    OP_GETGLOBAL,  // R[A] = Gbl[K[B]]
    OP_SETGLOBAL,  // Gbl[K[B]] = R[A]

    /* Table */
    OP_NEWTABLE,   // R[A] = {}
    OP_GETTABLE,   // R[A] = R[B][R[C]]
    OP_SETTABLE,   // R[A][R[B]] = R[C]
    OP_SETTABLEK,  // R[A][K[B]] = R[C]

    /* 算术: R[A] = R[B] op R[C] */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,

    /* 比较: R[A] = (R[B] op R[C]) */
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,

    /* 一元: R[A] = op R[B] */
    OP_NEG, OP_NOT, OP_LEN,

    /* 连接: R[A] = R[B] .. R[C] */
    OP_CONCAT,

    /* 控制流 */
    OP_JMP,        // pc += B（相对跳转）
    OP_TEST,       // if not is_truthy(R[A]) then pc += B
    OP_TESTN,      // if is_truthy(R[A]) then pc += B

    /* 函数 */
    OP_CALL,       // R[A] = R[A](R[A+1..A+B])
    OP_RETURN,     // return R[A]
    OP_CLOSURE,    // R[A] = closure(Proto[B])
} OpCode;
```

共 30 条指令。官方 38 条，缺少的指令见 [对照官方](../vs-official/)。

### 指令格式

```c
typedef struct {
    OpCode op;
    int A, B, C;  // 三个操作数
    int line;     // 源码行号
} Instruction;
```

官方用 32 位紧凑编码（Op:6 A:8 B:9 C:9），我们用 struct 更直观但占内存多。教学优先可读性。

## 寄存器布局

```
R[0..nparams-1]        — 函数参数
R[nparams..nlocals-1]  — 局部变量
R[nlocals..nregs-1]    — 表达式求值的临时寄存器
```

### safe_reg 机制

编译器分配临时寄存器时不能覆盖局部变量：

```c
int safe_reg(Compiler *c) {
    return c->next_reg > c->nlocals ? c->next_reg : c->nlocals;
}
```

每次表达式求值后 `c->next_reg` 回退，临时寄存器可复用。

## 编译器改造

### 编译表达式 → 返回结果寄存器

```c
int compile_exp(Compiler *c, Exp *e) {
    switch (e->type) {
        case EXP_NUMBER: {
            int reg = safe_reg(c);
            emit(c, OP_LOADK, reg, add_constant(c, make_number(e->u.number)));
            return reg;
        }
        case EXP_BINOP: {
            int lr = compile_exp(c, e->u.binop.left);
            int rr = compile_exp(c, e->u.binop.right);
            int dr = safe_reg(c);
            emit(c, binop_to_op(e->u.binop.op), dr, lr, rr);
            return dr;
        }
    }
}
```

与栈式编译器的区别：不再隐式 push/pop，而是显式管理寄存器编号。

### for 循环展开

官方有 `OP_FORLOOP`/`OP_FORPREP` 专用指令。我们编译为 `while` + 计数器：

```c
// for i = 1, 10 do ... end
// 编译为：
local i = 1
while i <= 10 do
    ...body...
    i = i + 1
end
```

更简单但指令更多。OP_FORLOOP 的优势是单条指令完成比较+递增+跳转。

## 相对跳转

阶段 2 的 `OP_JMP target`（绝对）→ 阶段 4 的 `OP_JMP offset`（相对）：

```c
case OP_JMP:
    frame->ip += B;  // 相对当前 PC 跳转
    break;
```

相对跳转的好处：
1. 代码移动不受影响——在前面插入指令不需要修正跳转目标
2. 闭包共享的 Proto 可以在不同位置加载，跳转偏移不变

## VM 主循环

```c
lua_Value call_function(lua_State *L, Function *fn, lua_Value *args, int nargs) {
    if (fn->is_builtin) return fn->u.builtin(args, nargs);

    // 创建调用帧
    CallFrame *frame = &L->frames[L->frame_count++];
    frame->fn = fn;
    frame->ip = 0;
    int nregs = p->nregs > 0 ? p->nregs : 1;
    frame->regs = calloc(nregs, sizeof(lua_Value));

    // 绑定参数
    for (int i = 0; i < p->nparams; i++)
        frame->regs[i] = (i < nargs) ? args[i] : make_nil();

    lua_Value *R = frame->regs;
    while (frame->ip < p->code_size) {
        Instruction inst = p->code[frame->ip++];
        switch (inst.op) {
            case OP_ADD:
                R[inst.A] = do_arith(OP_ADD, R[inst.B], R[inst.C]);
                break;
            case OP_CALL:
                R[inst.A] = call_function(L, R[inst.A].v.fn, &R[inst.A+1], inst.B);
                break;
            case OP_RETURN:
                return R[inst.A];
        }
    }
}
```

## 对照官方

| 方面 | 官方 38 条 | 我们 30 条 |
|------|-----------|-----------|
| OP_SELF | method call 语法糖 | GETTABLE + CALL |
| OP_TAILCALL | 尾调用优化 | 不支持 |
| OP_FORLOOP | for 专用 | while + 计数器 |
| OP_SETLIST | table 批量构造 | 逐条 SETTABLEK |
| OP_VARARG | 可变参数 | 不支持 |
| OP_TESTSET | TEST + MOVE 合并 | TEST + MOVE |

## 性能

```
benchmark: fib(30) + 100万累加 + 10万table ~0.6s
```

寄存器式比栈式快约 30-50%，主要因为：
1. 省去 push/pop 的间接栈访问
2. 局部变量直接在寄存器，不需要每次 GETLOCAL/SETLOCAL
3. 指令数更少

## 测试

```
测试 01-06: 全部 ✓
```

与阶段 3 相同的测试结果，但内部执行寄存器式字节码。

## 从阶段 3 到阶段 4 的改动

| 文件 | 改动 |
|------|------|
| lopcodes.h | **重写**：栈式指令 → 寄存器式指令 |
| lcode.h/c | **重写**：栈式编译 → 寄存器式编译（safe_reg） |
| lvm.h/c | **重写**：值栈 → 寄存器数组，push/pop → R[A]/R[B]/R[C] |
| lobject.h | Proto 加 nregs 字段 |
| ltable.h/c | 不变 |
| ltm.h/c | 不变 |
| lbaselib.c | 微调：参数从栈传递改为数组传递 |

关键：table 和 metatable 的实现完全不变——它们是数据结构层面的，与 VM 是栈式还是寄存器式无关。变的是指令集和编译器。

## 寄存器分配策略

我们的寄存器分配很简单——线性扫描，临时寄存器用完即回退：

```c
// 编译 a + b * c
int r1 = compile_exp(c, a);     // r1 = 0 (局部变量 a)
int r2 = compile_exp(c, b);     // r2 = 1 (局部变量 b)
int r3 = compile_exp(c, c);     // r3 = 2 (局部变量 c)
emit(c, OP_MUL, 3, r2, r3);     // R[3] = R[1] * R[2]
emit(c, OP_ADD, 0, r1, 3);      // R[0] = R[0] + R[3]
```

官方 Lua 的寄存器分配更精细——会复用临时寄存器，减少 nregs。我们简化为线性分配，nregs 足够大就行。

## OP_CALL 的参数传递

寄存器式 OP_CALL 的参数放在连续寄存器中：

```
OP_CALL A B    →  R[A] = R[A](R[A+1], R[A+2], ..., R[A+B])
```

调用前编译器把参数依次放到 R[A+1..A+B]，调用后返回值放回 R[A]。这要求参数寄存器连续，编译器需要规划好寄存器布局。

## OP_CLOSURE 与 upvalue 捕获

```c
case OP_CLOSURE: {
    Proto *sub = p->subprotos[B];
    Function *closure = create_closure(sub, frame, L);
    R[A] = make_function(closure);
    break;
}
```

`create_closure` 根据 Proto 的 upvalue 描述创建 UpVal 引用：
- `is_local=1` → `find_or_create_open_upval(&frame->regs[index])`
- `is_local=0` → `frame->fn->upvals[index]`（复用父闭包的 UpVal）
