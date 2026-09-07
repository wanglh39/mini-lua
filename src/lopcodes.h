/*
 * lopcodes.h - 寄存器式字节码指令集（OP_MOVE / OP_LOADK / OP_ADD 等）
 * 官方对照: lua-5.1.5/src/lopcodes.h
 * 实现阶段: 阶段 4+
 *
 * 阶段 4：寄存器式 VM（对齐官方 Lua 5.1 的指令集设计）。
 *
 * 栈式 vs 寄存器式：
 *   栈式：每条指令从栈顶取操作数，结果压回栈顶。
 *         ADD → 弹出 b, 弹出 a, 压入 a+b。需要大量 PUSH/POP。
 *   寄存器式：每条指令指定源和目标寄存器。
 *         OP_ADD A B C → R[A] = R[B] + R[C]。无需 PUSH/POP。
 *
 * 寄存器式优势：
 *   1. 指令更少（省去 PUSH/POP）
 *   2. 栈访问更少（直接读写寄存器）
 *   3. 更适合现代 CPU（寄存器分配友好）
 *
 * 寄存器布局：
 *   R[0..nparams-1]        — 函数参数
 *   R[nparams..nlocals-1]  — 局部变量
 *   R[nlocals..nregs-1]    — 表达式求值的临时寄存器
 *
 * 指令格式：每条指令 = OpCode + A + B + C（三个 int 操作数）。
 *   官方 Lua 用 32 位紧凑编码（A:8 B:9 C:9），我们用 struct 更直观。
 */

#ifndef lopcodes_h
#define lopcodes_h

typedef enum {
    /* ─── 值加载到寄存器 ─── */
    OP_LOADK,      /* R[A] = K[B]              (B = 常量表索引) */
    OP_LOADNIL,    /* R[A] = nil */
    OP_LOADBOOL,   /* R[A] = (B != 0)          (B=0→false, B=1→true) */

    /* ─── 寄存器复制 ─── */
    OP_MOVE,       /* R[A] = R[B] */

    /* ─── upvalue 访问 ─── */
    OP_GETUPVAL,   /* R[A] = UpVal[B]          (B = upvalue 索引) */
    OP_SETUPVAL,   /* UpVal[B] = R[A] */

    /* ─── 全局变量访问 ─── */
    OP_GETGLOBAL,  /* R[A] = Gbl[K[B]]         (B = 常量表索引) */
    OP_SETGLOBAL,  /* Gbl[K[B]] = R[A] */

    /* ─── Table 操作 ─── */
    OP_NEWTABLE,   /* R[A] = {} */
    OP_GETTABLE,   /* R[A] = R[B][R[C]]        (B=table, C=key) */
    OP_SETTABLE,   /* R[A][R[B]] = R[C]        (A=table, B=key, C=value) */
    OP_SETTABLEK,  /* R[A][K[B]] = R[C]        (A=table, B=常量索引, C=value) */

    /* ─── 二元算术: R[A] = R[B] op R[C] ─── */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,

    /* ─── 比较运算: R[A] = (R[B] op R[C]) → 布尔结果 ─── */
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,

    /* ─── 一元运算: R[A] = op R[B] ─── */
    OP_NEG, OP_NOT, OP_LEN,

    /* ─── 字符串连接: R[A] = R[B] .. R[C] ─── */
    OP_CONCAT,

    /* ─── 控制流 ─── */
    OP_JMP,        /* pc += B                  (B = 相对跳转偏移) */
    OP_TEST,       /* if not is_truthy(R[A]) then pc += B
                                                 * if/while 条件、and 短路 */
    OP_TESTN,      /* if is_truthy(R[A]) then pc += B
                                                 * or 短路求值 */

    /* ─── 函数 ─── */
    OP_CALL,       /* R[A] = R[A](R[A+1..A+B])  (B = 参数个数) */
    OP_RETURN,     /* return R[A] */
    OP_CLOSURE,    /* R[A] = closure(Proto[B])  (B = 子 Proto 索引) */

} OpCode;

/* 指令结构：寄存器式，三个操作数 A/B/C */
typedef struct Instruction {
    OpCode op;
    int A, B, C;
    int line;
} Instruction;

const char *op_name(OpCode op);

#endif /* lopcodes_h */
