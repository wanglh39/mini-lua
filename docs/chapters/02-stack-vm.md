# 阶段 2：栈式字节码 VM

> 源码目录：`stage2/`
> 官方对照：lua-5.1.5/src/lvm.c（栈式部分）
> 测试：01-03 通过，04-06 报"阶段 2 不支持 table"

## 目标

引入编译器（lcode.c）和指令集（lopcodes.h），生成字节码再执行。理解编译和执行分离的好处。

阶段 1 的树遍历解释器有个问题：每次执行都要遍历 AST，重复工作。而且变量查找是 O(n) 的链表遍历。阶段 2 把 AST 编译成字节码——线性指令数组，变量变成槽位索引，O(1) 访问。

## 架构

```
源码 → llex → lparser(AST) → lcode(字节码) → lvm 栈式 VM 执行
```

执行流程变成两步：编译时 `lcode.c` 遍历 AST 生成 `Instruction[]`，运行时 `lvm.c` 逐条执行指令。

## 指令集设计

栈式 VM 的核心：每条指令从值栈顶取操作数，结果压回栈顶。

```c
typedef enum {
    OP_NIL, OP_NUMBER, OP_STRING, OP_BOOL,
    OP_GETLOCAL, OP_SETLOCAL, OP_GETUPVAL, OP_SETUPVAL,
    OP_GETGLOBAL, OP_SETGLOBAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_NEG, OP_NOT, OP_LEN,
    OP_JMP, OP_JIF, OP_CALL, OP_RETURN, OP_CLOSURE,
} OpCode;
```

### 栈式执行示例

计算 `3 + 4 * 5`：

```
OP_NUMBER 3    → stack: [3]
OP_NUMBER 4    → stack: [3, 4]
OP_NUMBER 5    → stack: [3, 4, 5]
OP_MUL         → stack: [3, 20]    (弹出 4,5，压入 20)
OP_ADD         → stack: [23]       (弹出 3,20，压入 23)
```

每条算术指令弹两个压一个，栈顶始终是当前表达式的结果。

## 编译器 (lcode.c)

编译器遍历 AST，为每个函数生成 `Proto`（字节码 + 元数据）：

```c
struct Proto {
    Instruction *code;      // 指令数组
    int code_size;
    lua_Value *constants;   // 常量表
    int nconstants;
    char **local_names;     // 局部变量名表
    int nlocals;
    int nparams;            // 参数个数
    Proto **subprotos;      // 内嵌子函数
    int nsubprotos;
    UpvalDesc *upvals;      // upvalue 描述
    int nupvals;
};
```

### 编译表达式

```c
void compile_exp(Compiler *c, Exp *e) {
    switch (e->type) {
        case EXP_NUMBER:
            emit(c, OP_NUMBER, add_constant(c, make_number(e->u.number)));
            break;
        case EXP_BINOP:
            compile_exp(c, e->u.binop.left);   // 左操作数压栈
            compile_exp(c, e->u.binop.right);  // 右操作数压栈
            emit(c, binop_to_op(e->u.binop.op)); // 弹两个压一个
            break;
        case EXP_CALL:
            compile_exp(c, e->u.call.fn);       // 函数压栈
            for (ExpList *el = e->u.call.args; el; el = el->tail)
                compile_exp(c, el->head);       // 参数依次压栈
            emit(c, OP_CALL, nargs);
            break;
    }
}
```

### 跳转回填

条件跳转的目标在编译 `if` 时还不知道——then 分支还没编译完。用**回填**技术：

```c
void compile_if(Compiler *c, Stmt *s) {
    compile_exp(c, s->u.if_.cond);
    int jmp_to_else = emit(c, OP_JIF, 0);  // 目标待填
    compile_block(c, s->u.if_.then_branch);
    if (s->u.if_.else_branch) {
        int jmp_to_end = emit(c, OP_JMP, 0);
        patch(c, jmp_to_else, current_pc(c));  // 回填 else 起始
        compile_block(c, s->u.if_.else_branch);
        patch(c, jmp_to_end, current_pc(c));   // 回填 end
    } else {
        patch(c, jmp_to_else, current_pc(c));  // 回填 end
    }
}
```

## 栈式 VM (lvm.c)

### 值栈与调用帧

```c
struct lua_State {
    lua_Value *stack;    // 值栈
    int top;             // 栈顶
    CallFrame *frames;   // 调用帧栈
    int frame_count;
};

struct CallFrame {
    Function *fn;
    lua_Value *locals;   // 局部变量数组（独立于值栈）
    int base;            // 在值栈中的基准位置
};
```

### 主循环

```c
while (pc < p->code_size) {
    Instruction inst = p->code[pc++];
    switch (inst.op) {
        case OP_ADD: {
            lua_Value b = pop(L), a = pop(L);
            push(L, make_number(a.v.n + b.v.n));
            break;
        }
        case OP_CALL: {
            lua_Value fn_val = pop(L);
            lua_Value *args = &L->stack[L->top - inst.B];
            lua_Value result = call_function(L, fn_val.v.fn, args, inst.B);
            L->top -= inst.B;
            push(L, result);
            break;
        }
        case OP_RETURN:
            return pop(L);
    }
}
```

## 关键决策

### 绝对跳转 vs 相对跳转

阶段 2 用绝对跳转（`OP_JMP target`，target 是目标指令索引）。阶段 4 改为相对跳转（`OP_JMP offset`，offset 相对当前 PC）——更适合代码移动和闭包。

绝对跳转的问题：如果在中间插入指令，后面所有跳转目标都要修正。相对跳转不受影响。

### 帧指针 realloc 失效

`L->frames` 可能 realloc 扩容，导致 `CallFrame *frame` 指针失效。修复：用 `frame_idx` 索引而非指针：

```c
// 错误：frame 指针在 realloc 后失效
CallFrame *frame = &L->frames[L->frame_count++];
// ... realloc 可能发生 ...
frame->local[i] = ...;  // UB！

// 正确：用索引重新获取
int frame_idx = L->frame_count++;
L->frames[frame_idx].locals[i] = ...;  // 安全
```

## 对照官方

Lua 5.0 及之前是栈式 VM，5.1 起改为寄存器式。我们阶段 2→4 复现了这个演进。

| 方面 | 官方 Lua 5.0 | 我们阶段 2 |
|------|-------------|-----------|
| 指令格式 | 32 位紧凑 | struct |
| 跳转 | 绝对 | 绝对 |
| 帧管理 | CallInfo 数组 | CallFrame 数组 |
| 编译 | 单遍 | 两遍（先 AST） |

## 编译 vs 解释的权衡

阶段 1（树遍历）vs 阶段 2（字节码）的核心区别：

| | 树遍历 | 字节码 |
|---|--------|--------|
| 执行方式 | 递归遍历 AST | 线性执行指令数组 |
| 变量查找 | 环境链表 O(n) | 槽位索引 O(1) |
| 重复执行 | 每次遍历 AST | 只编译一次 |
| 内存 | AST 常驻 | Proto 可序列化 |
| 调试 | AST 有完整信息 | 需要 LineInfo |

树遍历适合开发期（快速原型），字节码适合运行期（高效执行）。很多语言先实现树遍历再换成字节码——Python、Ruby、JavaScript V8 都有这个演进。

## 栈式 VM 的 push/pop 模式

栈式 VM 的所有操作都围绕栈顶：

```
表达式 a + b * c:

编译：              执行（栈状态）：
PUSH a              [a]
PUSH b              [a, b]
PUSH c              [a, b, c]
MUL                 [a, b*c]
ADD                 [a+b*c]
```

优点：指令不需要指定操作数位置——隐式从栈顶取。
缺点：大量 push/pop 间接访问，局部变量也要 GETLOCAL/SETLOCAL。

## 闭包编译

函数字面量编译为 OP_CLOSURE 指令：

```c
case EXP_FUNCTION: {
    Proto *sub = compile_function(c, e);  // 递归编译子函数
    int idx = add_subproto(c, sub);
    emit(c, OP_CLOSURE, idx);  // push closure(Proto[idx])
    break;
}
```

upvalue 描述在编译时确定：
- `is_local=1`：指向直接外层的局部变量（栈槽）
- `is_local=0`：指向直接外层的 upvalue（隔层捕获）

运行时 `create_closure` 根据描述创建 UpVal 引用。

## 测试

```
测试 01-03: ✓  (与阶段 1 相同结果，但内部执行字节码)
测试 04-06: ✗  "阶段 2 不支持 table"
```

## 从阶段 1 到阶段 2 的改动

| 文件 | 改动 |
|------|------|
| lobject.h | 加入 Proto/Instruction/OpCode，Function 改为闭包 |
| lopcodes.h | **新增**：指令集定义 |
| lcode.h/c | **新增**：编译器（AST → 字节码） |
| lvm.h/c | **重写**：eval_exp → call_function + VM 主循环 |
| llex.h/c | 不变 |
| lparser.h/c | 不变 |
| lbaselib.c | 微调：适配新的 Function 结构 |

关键：llex 和 lparser 完全复用——语法分析不变，只是 AST 之后的处理从"直接执行"变成"编译再执行"。
