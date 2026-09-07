/*
 * lopcodes.h - 栈式字节码指令集
 * 官方对照: lua-5.1.5/src/lopcodes.h
 * 实现阶段: 阶段 2+
 *
 * 阶段 2/3：栈式字节码 VM。
 *
 * 栈式 vs 寄存器式：
 *   栈式：每条指令从栈顶取操作数，结果压回栈顶。
 *         ADD → 弹出 b, 弹出 a, 压入 a+b。需要大量 PUSH/POP。
 *   寄存器式（阶段 4）：每条指令指定源和目标寄存器。
 *         OP_ADD A B C → R[A] = R[B] + R[C]。无需 PUSH/POP。
 *
 * 指令格式：每条指令 = OpCode + arg（一个 int 操作数）。
 *   阶段 4 改为 A/B/C 三操作数。
 */

#ifndef lopcodes_h
#define lopcodes_h

typedef enum {
    /* ─── 值压栈 ─── */
    OP_NIL,        /* push nil */
    OP_TRUE,       /* push true */
    OP_FALSE,      /* push false */
    OP_NUMBER,     /* push constants[arg] */
    OP_STRING,     /* push constants[arg] */

    /* ─── 局部变量 ─── */
    OP_GETLOCAL,   /* push locals[arg] */
    OP_SETLOCAL,   /* locals[arg] = pop() */
    OP_NEWLOCAL,   /* 声明新局部变量（阶段 2 预留） */

    /* ─── upvalue ─── */
    OP_GETUPVAL,   /* push *upvals[arg]->ptr */
    OP_SETUPVAL,   /* *upvals[arg]->ptr = pop() */

    /* ─── 全局变量 ─── */
    OP_GETGLOBAL,  /* push globals[constants[arg]] */
    OP_SETGLOBAL,  /* globals[constants[arg]] = pop() */

    /* ─── 二元算术：弹出 b, a，压入 a op b ─── */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,

    /* ─── 字符串连接 ─── */
    OP_CONCAT,

    /* ─── 比较运算：弹出 b, a，压入 boolean(a op b) ─── */
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,

    /* ─── 一元运算：弹出 a，压入 op a ─── */
    OP_NOT, OP_NEG, OP_LEN,

    /* ─── 控制流 ─── */
    OP_JUMP,       /* ip = arg（绝对跳转） */
    OP_JUMPF,      /* if not is_truthy(peek()) then ip = arg（不弹出） */
    OP_JUMPT,      /* if is_truthy(peek()) then ip = arg（不弹出） */
    OP_POP,        /* 弹出栈顶 */

    /* ─── 函数 ─── */
    OP_CALL,       /* call(arg = nargs)：弹出 fn 和 args，压入返回值 */
    OP_RETURN,     /* return pop() */
    OP_CLOSURE,    /* push closure(subprotos[arg]) */

    /* ─── Table 操作（阶段 3） ─── */
    OP_NEWTABLE,       /* push {} */
    OP_GETINDEX,       /* 弹出 key, table，压入 table[key]（带 __index） */
    OP_SETINDEX,       /* 弹出 value, key, table，table[key] = value（带 __newindex） */
    OP_SETINDEX_CONSTR, /* table 构造用：弹出 value, key，栈顶 table[key] = value */

} OpCode;

/* 指令结构：栈式，单操作数 arg */
typedef struct Instruction {
    OpCode op;
    int arg;
    int line;
} Instruction;

const char *op_name(OpCode op);

#endif /* lopcodes_h */
