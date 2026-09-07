/*
 * lcode.c - 栈式编译器（AST → 栈式字节码）
 * 官方对照: lua-5.1.5/src/lcode.c
 * 实现阶段: 阶段 2+
 *
 * 栈式编译核心：
 *   compile_exp(c, e) — 将表达式 e 的值压入值栈顶
 *   语句编译后值栈恢复原状（无残留）
 *
 * 对比阶段 4（寄存器式）：
 *   栈式：compile_exp(c, e) 压栈，无需指定目标寄存器。
 *   寄存器式：compile_exp(c, e, dest) 放入 R[dest]，需寄存器分配。
 *
 * 跳转指令用绝对地址（OP_JUMP arg = 目标指令下标）。
 *   阶段 4 改为相对跳转（pc += B）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lcode.h"

#define MAX_LOCALS 200
#define MAX_UPVALS 60

typedef struct LoopCtx {
    int *break_jumps;
    int nbreaks, break_cap;
    struct LoopCtx *parent;
} LoopCtx;

typedef struct Compiler {
    struct Compiler *parent;
    struct { char *name; int scope; } locals[MAX_LOCALS];
    int nlocals, scope_depth;
    Instruction *code;
    int code_size, code_cap;
    lua_Value *constants;
    int nconstants, const_cap;
    Proto **subprotos;
    int nsubprotos, subproto_cap;
    UpvalDesc upvals[MAX_UPVALS];
    char *upval_names[MAX_UPVALS];
    int nupvals, nparams;
    LoopCtx *loop;
} Compiler;

/* ─── 指令发射 ─── */

static void emit(Compiler *c, OpCode op, int arg, int line) {
    if (c->code_size >= c->code_cap) {
        c->code_cap = c->code_cap ? c->code_cap * 2 : 64;
        c->code = (Instruction *)realloc(c->code, c->code_cap * sizeof(Instruction));
    }
    c->code[c->code_size] = (Instruction){op, arg, line};
    c->code_size++;
}

static int emit_jump(Compiler *c, OpCode op, int line) {
    emit(c, op, 0, line);
    return c->code_size - 1;
}

/* 回填绝对跳转：arg = 当前 code_size（目标地址） */
static void patch_jump(Compiler *c, int jump_pos) {
    c->code[jump_pos].arg = c->code_size;
}

/* ─── 常量表 ─── */

static int add_constant(Compiler *c, lua_Value v) {
    for (int i = 0; i < c->nconstants; i++)
        if (value_equals(c->constants[i], v)) return i;
    if (c->nconstants >= c->const_cap) {
        c->const_cap = c->const_cap ? c->const_cap * 2 : 16;
        c->constants = (lua_Value *)realloc(c->constants, c->const_cap * sizeof(lua_Value));
    }
    c->constants[c->nconstants] = v;
    return c->nconstants++;
}

/* ─── 局部变量 ─── */

static int add_local(Compiler *c, const char *name) {
    if (c->nlocals >= MAX_LOCALS) { fprintf(stderr, "局部变量过多\n"); exit(1); }
    int slot = c->nlocals;
    c->locals[slot].name = strdup(name);
    c->locals[slot].scope = c->scope_depth;
    c->nlocals++;
    return slot;
}

static int find_local(Compiler *c, const char *name) {
    for (int i = c->nlocals - 1; i >= 0; i--)
        if (strcmp(c->locals[i].name, name) == 0) return i;
    return -1;
}

static int resolve_upval(Compiler *c, const char *name) {
    for (int i = 0; i < c->nupvals; i++)
        if (strcmp(c->upval_names[i], name) == 0) return i;
    if (!c->parent) return -1;
    int slot = find_local(c->parent, name);
    if (slot >= 0) {
        int idx = c->nupvals;
        c->upvals[idx].is_local = 1;
        c->upvals[idx].index = slot;
        c->upval_names[idx] = strdup(name);
        c->nupvals++;
        return idx;
    }
    int parent_uv = resolve_upval(c->parent, name);
    if (parent_uv >= 0) {
        int idx = c->nupvals;
        c->upvals[idx].is_local = 0;
        c->upvals[idx].index = parent_uv;
        c->upval_names[idx] = strdup(name);
        c->nupvals++;
        return idx;
    }
    return -1;
}

static void enter_scope(Compiler *c) { c->scope_depth++; }
static void leave_scope(Compiler *c) {
    c->scope_depth--;
    while (c->nlocals > 0 && c->locals[c->nlocals - 1].scope > c->scope_depth)
        c->nlocals--;
}

/* ─── break ─── */

static void push_loop(Compiler *c, LoopCtx *ctx) {
    ctx->break_jumps = NULL; ctx->nbreaks = 0; ctx->break_cap = 0;
    ctx->parent = c->loop; c->loop = ctx;
}
static void pop_loop(Compiler *c) { c->loop = c->loop->parent; }
static void emit_break(Compiler *c, int line) {
    if (!c->loop) { fprintf(stderr, "break 不在循环内\n"); exit(1); }
    int pos = emit_jump(c, OP_JUMP, line);
    LoopCtx *ctx = c->loop;
    if (ctx->nbreaks >= ctx->break_cap) {
        ctx->break_cap = ctx->break_cap ? ctx->break_cap * 2 : 8;
        ctx->break_jumps = (int *)realloc(ctx->break_jumps, ctx->break_cap * sizeof(int));
    }
    ctx->break_jumps[ctx->nbreaks++] = pos;
}
static void patch_breaks(Compiler *c) {
    LoopCtx *ctx = c->loop;
    for (int i = 0; i < ctx->nbreaks; i++) patch_jump(c, ctx->break_jumps[i]);
    free(ctx->break_jumps);
}

/* ─── 前向声明 ─── */
static void compile_exp(Compiler *c, Exp *e);
static void compile_stat(Compiler *c, Stmt *s);
static void compile_block(Compiler *c, Stmt *block);
static Proto *compile_function(Stmt *body, char **params, int nparams, Compiler *parent);

/* ─── 表达式编译：结果压入值栈顶 ─── */

static void compile_exp(Compiler *c, Exp *e) {
    switch (e->type) {
        case EXP_NIL:
            emit(c, OP_NIL, 0, e->line);
            break;

        case EXP_BOOL:
            emit(c, e->u.boolean ? OP_TRUE : OP_FALSE, 0, e->line);
            break;

        case EXP_NUMBER:
            emit(c, OP_NUMBER, add_constant(c, make_number(e->u.number)), e->line);
            break;

        case EXP_STRING:
            emit(c, OP_STRING, add_constant(c, make_string(e->u.string)), e->line);
            break;

        case EXP_VAR: {
            int slot = find_local(c, e->u.var);
            if (slot >= 0) emit(c, OP_GETLOCAL, slot, e->line);
            else {
                int uv = resolve_upval(c, e->u.var);
                if (uv >= 0) emit(c, OP_GETUPVAL, uv, e->line);
                else emit(c, OP_GETGLOBAL, add_constant(c, make_string(e->u.var)), e->line);
            }
            break;
        }

        case EXP_BINOP: {
            /* and/or 短路求值 */
            if (e->u.binop.op == TK_AND) {
                /* a and b: 如果 a 为假，跳过 b，a 留在栈顶作为结果 */
                compile_exp(c, e->u.binop.left);
                int j = emit_jump(c, OP_JUMPF, e->line);  /* a 为假时跳转，a 留在栈顶 */
                emit(c, OP_POP, 0, e->line);               /* a 为真，弹出 a，求值 b */
                compile_exp(c, e->u.binop.right);
                patch_jump(c, j);                          /* 跳转目标：a 在栈顶 */
                return;
            }
            if (e->u.binop.op == TK_OR) {
                /* a or b: 如果 a 为真，跳过 b，a 留在栈顶作为结果 */
                compile_exp(c, e->u.binop.left);
                int j = emit_jump(c, OP_JUMPT, e->line);  /* a 为真时跳转，a 留在栈顶 */
                emit(c, OP_POP, 0, e->line);               /* a 为假，弹出 a，求值 b */
                compile_exp(c, e->u.binop.right);
                patch_jump(c, j);
                return;
            }

            /* 普通二元运算：压入 left, right，然后运算 */
            compile_exp(c, e->u.binop.left);
            compile_exp(c, e->u.binop.right);

            OpCode op;
            switch (e->u.binop.op) {
                case TK_PLUS:    op = OP_ADD; break;
                case TK_MINUS:   op = OP_SUB; break;
                case TK_STAR:    op = OP_MUL; break;
                case TK_SLASH:   op = OP_DIV; break;
                case TK_PERCENT: op = OP_MOD; break;
                case TK_CARET:   op = OP_POW; break;
                case TK_CONCAT:  op = OP_CONCAT; break;
                case TK_EQ:      op = OP_EQ; break;
                case TK_NE:      op = OP_NE; break;
                case TK_LT:      op = OP_LT; break;
                case TK_GT:      op = OP_GT; break;
                case TK_LE:      op = OP_LE; break;
                case TK_GE:      op = OP_GE; break;
                default: fprintf(stderr, "未知运算符\n"); exit(1);
            }
            emit(c, op, 0, e->line);
            break;
        }

        case EXP_UNOP:
            compile_exp(c, e->u.unop.operand);
            switch (e->u.unop.op) {
                case TK_MINUS: emit(c, OP_NEG, 0, e->line); break;
                case TK_NOT:   emit(c, OP_NOT, 0, e->line); break;
                case TK_LEN:   emit(c, OP_LEN, 0, e->line); break;
                default: fprintf(stderr, "未知一元运算符\n"); exit(1);
            }
            break;

        case EXP_CALL: {
            /* 压入 fn, arg1, arg2, ..., argN，然后 OP_CALL(nargs) */
            compile_exp(c, e->u.call.fn);
            int nargs = 0;
            for (ExpList *l = e->u.call.args; l; l = l->tail) {
                compile_exp(c, l->head);
                nargs++;
            }
            emit(c, OP_CALL, nargs, e->line);
            break;
        }

        case EXP_FUNCTION: {
            Proto *p = compile_function(e->u.func.body, e->u.func.params,
                                        e->u.func.nparams, c);
            if (c->nsubprotos >= c->subproto_cap) {
                c->subproto_cap = c->subproto_cap ? c->subproto_cap * 2 : 8;
                c->subprotos = (Proto **)realloc(c->subprotos, c->subproto_cap * sizeof(Proto *));
            }
            int idx = c->nsubprotos;
            c->subprotos[c->nsubprotos++] = p;
            emit(c, OP_CLOSURE, idx, e->line);
            break;
        }

        case EXP_TABLE:
            fprintf(stderr, "编译错误（第 %d 行）：阶段 2 不支持 table 构造\n", e->line);
            exit(1);

        case EXP_INDEX:
            fprintf(stderr, "编译错误（第 %d 行）：阶段 2 不支持索引访问\n", e->line);
            exit(1);
    }
}

/* ─── 语句编译 ─── */

static void compile_stat(Compiler *c, Stmt *s) {
    switch (s->type) {
        case STMT_EXPR:
            compile_exp(c, s->u.expr);
            emit(c, OP_POP, 0, s->line);  /* 表达式语句：弹出结果 */
            break;

        case STMT_LOCAL: {
            compile_exp(c, s->u.local.value);
            int slot = add_local(c, s->u.local.name);
            emit(c, OP_SETLOCAL, slot, s->line);
            break;
        }

        case STMT_ASSIGN: {
            compile_exp(c, s->u.assign.value);
            int slot = find_local(c, s->u.assign.name);
            if (slot >= 0) emit(c, OP_SETLOCAL, slot, s->line);
            else {
                int uv = resolve_upval(c, s->u.assign.name);
                if (uv >= 0) emit(c, OP_SETUPVAL, uv, s->line);
                else emit(c, OP_SETGLOBAL, add_constant(c, make_string(s->u.assign.name)), s->line);
            }
            break;
        }

        case STMT_INDEX_ASSIGN:
            fprintf(stderr, "编译错误（第 %d 行）：阶段 2 不支持索引赋值\n", s->line);
            exit(1);

        case STMT_IF: {
            /*
             * 栈式 if 编译：
             *   compile cond        ; push cond
             *   OP_JUMPF to L1      ; if false, jump to else. cond 留在栈顶
             *   OP_POP              ; pop cond (true case)
             *   compile then
             *   OP_JUMP to L2       ; skip else
             *   L1: OP_POP          ; pop cond (false case, from JUMPF)
             *   compile else
             *   L2:
             */
            compile_exp(c, s->u.if_.cond);
            int jf = emit_jump(c, OP_JUMPF, s->line);
            emit(c, OP_POP, 0, s->line);
            compile_block(c, s->u.if_.then_branch);
            int je = emit_jump(c, OP_JUMP, s->line);
            patch_jump(c, jf);
            emit(c, OP_POP, 0, s->line);
            if (s->u.if_.else_branch) compile_block(c, s->u.if_.else_branch);
            patch_jump(c, je);
            break;
        }

        case STMT_WHILE: {
            /*
             * L0: compile cond     ; push cond
             *     OP_JUMPF to L1    ; if false, jump to end. cond 留在栈顶
             *     OP_POP            ; pop cond
             *     compile body
             *     OP_JUMP to L0     ; loop back
             * L1: OP_POP            ; pop cond (loop exit)
             */
            int start = c->code_size;
            compile_exp(c, s->u.while_.cond);
            int jx = emit_jump(c, OP_JUMPF, s->line);
            emit(c, OP_POP, 0, s->line);
            enter_scope(c);
            LoopCtx ctx; push_loop(c, &ctx);
            compile_block(c, s->u.while_.body);
            patch_breaks(c); pop_loop(c);
            leave_scope(c);
            emit(c, OP_JUMP, start, s->line);
            patch_jump(c, jx);
            emit(c, OP_POP, 0, s->line);
            break;
        }

        case STMT_FOR: {
            /*
             * for var = start, end, step do body end
             * 编译为：
             *   var = start; limit = end; step = step or 1
             *   L0: if var <= limit then (或 >= 如果 step < 0)
             *       body
             *       var = var + step
             *       jump L0
             *   L1:
             */
            int var = add_local(c, s->u.for_.var);
            int lim = add_local(c, "(for limit)");
            int stp = add_local(c, "(for step)");
            compile_exp(c, s->u.for_.start);
            emit(c, OP_SETLOCAL, var, s->line);
            compile_exp(c, s->u.for_.end);
            emit(c, OP_SETLOCAL, lim, s->line);
            if (s->u.for_.step) {
                compile_exp(c, s->u.for_.step);
                emit(c, OP_SETLOCAL, stp, s->line);
            } else {
                emit(c, OP_NUMBER, add_constant(c, make_number(1)), s->line);
                emit(c, OP_SETLOCAL, stp, s->line);
            }

            int isneg = 0;
            if (s->u.for_.step) {
                Exp *sp = s->u.for_.step;
                if (sp->type == EXP_NUMBER && sp->u.number < 0) isneg = 1;
                else if (sp->type == EXP_UNOP && sp->u.unop.op == TK_MINUS
                         && sp->u.unop.operand->type == EXP_NUMBER
                         && sp->u.unop.operand->u.number > 0) isneg = 1;
            }

            int start = c->code_size;
            /* 编译比较：var <= limit 或 limit <= var（负步长） */
            if (isneg) {
                emit(c, OP_GETLOCAL, lim, s->line);
                emit(c, OP_GETLOCAL, var, s->line);
            } else {
                emit(c, OP_GETLOCAL, var, s->line);
                emit(c, OP_GETLOCAL, lim, s->line);
            }
            emit(c, OP_LE, 0, s->line);
            int jx = emit_jump(c, OP_JUMPF, s->line);
            emit(c, OP_POP, 0, s->line);

            enter_scope(c);
            LoopCtx ctx; push_loop(c, &ctx);
            compile_block(c, s->u.for_.body);
            patch_breaks(c); pop_loop(c);
            leave_scope(c);

            /* var = var + step */
            emit(c, OP_GETLOCAL, var, s->line);
            emit(c, OP_GETLOCAL, stp, s->line);
            emit(c, OP_ADD, 0, s->line);
            emit(c, OP_SETLOCAL, var, s->line);

            emit(c, OP_JUMP, start, s->line);
            patch_jump(c, jx);
            emit(c, OP_POP, 0, s->line);
            break;
        }

        case STMT_RETURN: {
            if (s->u.return_value) compile_exp(c, s->u.return_value);
            else emit(c, OP_NIL, 0, s->line);
            emit(c, OP_RETURN, 0, s->line);
            break;
        }

        case STMT_BREAK:
            emit_break(c, s->line);
            break;

        case STMT_FUNCTION: {
            Proto *p = compile_function(s->u.func.body, s->u.func.params,
                                        s->u.func.nparams, c);
            if (c->nsubprotos >= c->subproto_cap) {
                c->subproto_cap = c->subproto_cap ? c->subproto_cap * 2 : 8;
                c->subprotos = (Proto **)realloc(c->subprotos, c->subproto_cap * sizeof(Proto *));
            }
            int idx = c->nsubprotos;
            c->subprotos[c->nsubprotos++] = p;

            emit(c, OP_CLOSURE, idx, s->line);
            int slot = find_local(c, s->u.func.name);
            if (slot >= 0) emit(c, OP_SETLOCAL, slot, s->line);
            else {
                int uv = resolve_upval(c, s->u.func.name);
                if (uv >= 0) emit(c, OP_SETUPVAL, uv, s->line);
                else emit(c, OP_SETGLOBAL, add_constant(c, make_string(s->u.func.name)), s->line);
            }
            break;
        }
    }
}

static void compile_block(Compiler *c, Stmt *block) {
    enter_scope(c);
    for (Stmt *s = block; s; s = s->next) compile_stat(c, s);
    leave_scope(c);
}

static Proto *compile_function(Stmt *body, char **params, int nparams, Compiler *parent) {
    Compiler c;
    memset(&c, 0, sizeof(c));
    c.parent = parent;
    c.nparams = nparams;

    for (int i = 0; i < nparams; i++) add_local(&c, params[i]);
    for (Stmt *s = body; s; s = s->next) compile_stat(&c, s);

    /* 默认返回 nil */
    emit(&c, OP_NIL, 0, 0);
    emit(&c, OP_RETURN, 0, 0);

    Proto *p = (Proto *)calloc(1, sizeof(Proto));
    p->code = c.code;
    p->code_size = c.code_size;
    p->constants = c.constants;
    p->nconstants = c.nconstants;
    p->local_names = (char **)calloc(c.nlocals, sizeof(char *));
    for (int i = 0; i < c.nlocals; i++) p->local_names[i] = c.locals[i].name;
    p->nlocals = c.nlocals;
    p->nparams = nparams;
    p->subprotos = c.subprotos;
    p->nsubprotos = c.nsubprotos;
    if (c.nupvals > 0) {
        p->upvals = (UpvalDesc *)malloc(c.nupvals * sizeof(UpvalDesc));
        memcpy(p->upvals, c.upvals, c.nupvals * sizeof(UpvalDesc));
    }
    p->nupvals = c.nupvals;
    return p;
}

Proto *compile_main(Stmt *ast) {
    return compile_function(ast, NULL, 0, NULL);
}
