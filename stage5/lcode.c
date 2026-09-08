/*
 * lcode.c - 寄存器式编译器（AST → 寄存器字节码）
 * 官方对照: lua-5.1.5/src/lcode.c
 * 实现阶段: 阶段 4+
 *
 * 寄存器式编译核心：
 *   compile_exp(c, e, dest) — 将表达式 e 的值放到 R[dest]
 *   编译器跟踪 freereg（下一个可用临时寄存器）
 *
 * 寄存器分配规则：
 *   局部变量占用 R[0..nlocals-1]
 *   临时寄存器从 freereg 开始分配
 *   表达式编译时占用临时寄存器，完成后释放
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
    int freereg, maxregs;
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

static void emit(Compiler *c, OpCode op, int A, int B, int C, int line) {
    if (c->code_size >= c->code_cap) {
        c->code_cap = c->code_cap ? c->code_cap * 2 : 64;
        c->code = (Instruction *)realloc(c->code, c->code_cap * sizeof(Instruction));
    }
    c->code[c->code_size] = (Instruction){op, A, B, C, line};
    c->code_size++;
}

static int emit_jump(Compiler *c, OpCode op, int A, int line) {
    emit(c, op, A, 0, 0, line);
    return c->code_size - 1;
}

/* 回填相对跳转：B = target - (jump_pos + 1) */
static void patch_jump(Compiler *c, int jump_pos) {
    c->code[jump_pos].B = c->code_size - (jump_pos + 1);
}

static void reserve_reg(Compiler *c, int reg) {
    if (reg + 1 > c->maxregs) c->maxregs = reg + 1;
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
    reserve_reg(c, slot);
    if (c->freereg < c->nlocals) c->freereg = c->nlocals;
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
    c->freereg = c->nlocals;
}

/* ─── break ─── */

static void push_loop(Compiler *c, LoopCtx *ctx) {
    ctx->break_jumps = NULL; ctx->nbreaks = 0; ctx->break_cap = 0;
    ctx->parent = c->loop; c->loop = ctx;
}
static void pop_loop(Compiler *c) { c->loop = c->loop->parent; }
static void emit_break(Compiler *c, int line) {
    if (!c->loop) { fprintf(stderr, "break 不在循环内\n"); exit(1); }
    int pos = emit_jump(c, OP_JMP, 0, line);
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
static void compile_exp(Compiler *c, Exp *e, int dest);
static void compile_stat(Compiler *c, Stmt *s);
static void compile_block(Compiler *c, Stmt *block);
static Proto *compile_function(Stmt *body, char **params, int nparams, Compiler *parent);

/*
 * safe_reg — 确保寄存器号不与活跃局部变量冲突。
 *
 * 当表达式编译到 dest（一个局部变量槽位）时，临时寄存器 dest+1, dest+2...
 * 可能与其他局部变量重叠。此函数将临时寄存器抬升到 nlocals 以上。
 *
 * 例如：i=0, sum=1, nlocals=2，编译 `i = i + 1`（dest=0）：
 *   不修正：right = dest+1 = 1 → 覆盖 sum！
 *   修正后：right = safe_reg(1) = 2 → 使用 R[2]，不覆盖 sum。
 */
static int safe_reg(Compiler *c, int reg) {
    if (reg < c->nlocals) return c->nlocals;
    return reg;
}

/* ─── 表达式编译：R[dest] = e ─── */

static void compile_exp(Compiler *c, Exp *e, int dest) {
    reserve_reg(c, dest);

    switch (e->type) {
        case EXP_NIL:
            emit(c, OP_LOADNIL, dest, 0, 0, e->line);
            break;

        case EXP_BOOL:
            emit(c, OP_LOADBOOL, dest, e->u.boolean, 0, e->line);
            break;

        case EXP_NUMBER:
            emit(c, OP_LOADK, dest, add_constant(c, make_number(e->u.number)), 0, e->line);
            break;

        case EXP_STRING:
            emit(c, OP_LOADK, dest, add_constant(c, make_string(e->u.string)), 0, e->line);
            break;

        case EXP_VAR: {
            int slot = find_local(c, e->u.var);
            if (slot >= 0) {
                if (dest != slot) emit(c, OP_MOVE, dest, slot, 0, e->line);
            } else {
                int uv = resolve_upval(c, e->u.var);
                if (uv >= 0) emit(c, OP_GETUPVAL, dest, uv, 0, e->line);
                else emit(c, OP_GETGLOBAL, dest, add_constant(c, make_string(e->u.var)), 0, e->line);
            }
            break;
        }

        case EXP_BINOP: {
            /* and/or 短路求值 */
            if (e->u.binop.op == TK_AND) {
                compile_exp(c, e->u.binop.left, dest);
                int j = emit_jump(c, OP_TEST, dest, e->line);
                compile_exp(c, e->u.binop.right, dest);
                patch_jump(c, j);
                return;
            }
            if (e->u.binop.op == TK_OR) {
                compile_exp(c, e->u.binop.left, dest);
                int j = emit_jump(c, OP_TESTN, dest, e->line);
                compile_exp(c, e->u.binop.right, dest);
                patch_jump(c, j);
                return;
            }

            /* 二元运算：R[dest] = R[dest] op R[right] */
            compile_exp(c, e->u.binop.left, dest);
            int right = safe_reg(c, dest + 1);
            c->freereg = right + 1;
            compile_exp(c, e->u.binop.right, right);
            c->freereg = right;

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
            emit(c, op, dest, dest, right, e->line);
            break;
        }

        case EXP_UNOP:
            compile_exp(c, e->u.unop.operand, dest);
            switch (e->u.unop.op) {
                case TK_MINUS: emit(c, OP_NEG, dest, dest, 0, e->line); break;
                case TK_NOT:   emit(c, OP_NOT, dest, dest, 0, e->line); break;
                case TK_LEN:   emit(c, OP_LEN, dest, dest, 0, e->line); break;
                default: fprintf(stderr, "未知一元运算符\n"); exit(1);
            }
            break;

        case EXP_CALL: {
            compile_exp(c, e->u.call.fn, dest);
            int nargs = 0;
            int arg = safe_reg(c, dest + 1);
            c->freereg = arg;
            for (ExpList *l = e->u.call.args; l; l = l->tail) {
                compile_exp(c, l->head, arg);
                arg++; c->freereg = arg; nargs++;
            }
            c->freereg = safe_reg(c, dest + 1);
            emit(c, OP_CALL, dest, nargs, 0, e->line);
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
            emit(c, OP_CLOSURE, dest, idx, 0, e->line);
            break;
        }

        case EXP_TABLE: {
            emit(c, OP_NEWTABLE, dest, 0, 0, e->line);
            for (int i = 0; i < e->u.table.nentries; i++) {
                TableEntry *en = &e->u.table.entries[i];
                int key_reg = safe_reg(c, dest + 1);
                int val_reg = key_reg + 1;
                c->freereg = key_reg;
                compile_exp(c, en->key, key_reg);
                c->freereg = val_reg + 1;
                compile_exp(c, en->value, val_reg);
                c->freereg = key_reg;
                emit(c, OP_SETTABLE, dest, key_reg, val_reg, e->line);
            }
            break;
        }

        case EXP_INDEX: {
            compile_exp(c, e->u.index.obj, dest);
            int key_reg = safe_reg(c, dest + 1);
            c->freereg = key_reg + 1;
            compile_exp(c, e->u.index.key, key_reg);
            c->freereg = key_reg;
            emit(c, OP_GETTABLE, dest, dest, key_reg, e->line);
            break;
        }
    }
}

/* ─── 语句编译 ─── */

static void compile_stat(Compiler *c, Stmt *s) {
    switch (s->type) {
        case STMT_EXPR:
            compile_exp(c, s->u.expr, c->freereg);
            break;

        case STMT_LOCAL: {
            int slot = add_local(c, s->u.local.name);
            compile_exp(c, s->u.local.value, slot);
            c->freereg = slot + 1;
            break;
        }

        case STMT_ASSIGN: {
            int slot = find_local(c, s->u.assign.name);
            if (slot >= 0) {
                compile_exp(c, s->u.assign.value, slot);
            } else {
                int uv = resolve_upval(c, s->u.assign.name);
                int tmp = c->freereg;
                compile_exp(c, s->u.assign.value, tmp);
                if (uv >= 0) emit(c, OP_SETUPVAL, tmp, uv, 0, s->line);
                else emit(c, OP_SETGLOBAL, tmp, add_constant(c, make_string(s->u.assign.name)), 0, s->line);
            }
            break;
        }

        case STMT_INDEX_ASSIGN: {
            int base = c->freereg;
            c->freereg = base + 1;
            compile_exp(c, s->u.index_assign.obj, base);
            c->freereg = base + 2;
            compile_exp(c, s->u.index_assign.key, base + 1);
            c->freereg = base + 3;
            compile_exp(c, s->u.index_assign.value, base + 2);
            c->freereg = base;
            emit(c, OP_SETTABLE, base, base + 1, base + 2, s->line);
            break;
        }

        case STMT_IF: {
            int cond = c->freereg;
            compile_exp(c, s->u.if_.cond, cond);
            int jf = emit_jump(c, OP_TEST, cond, s->line);
            compile_block(c, s->u.if_.then_branch);
            int je = emit_jump(c, OP_JMP, 0, s->line);
            patch_jump(c, jf);
            if (s->u.if_.else_branch) compile_block(c, s->u.if_.else_branch);
            patch_jump(c, je);
            break;
        }

        case STMT_WHILE: {
            int start = c->code_size;
            int cond = c->freereg;
            compile_exp(c, s->u.while_.cond, cond);
            int jx = emit_jump(c, OP_TEST, cond, s->line);
            enter_scope(c);
            LoopCtx ctx; push_loop(c, &ctx);
            compile_block(c, s->u.while_.body);
            patch_breaks(c); pop_loop(c);
            leave_scope(c);
            emit(c, OP_JMP, 0, start - (c->code_size + 1), 0, s->line);
            patch_jump(c, jx);
            break;
        }

        case STMT_FOR: {
            int var = add_local(c, s->u.for_.var);
            int lim = add_local(c, "(for limit)");
            int stp = add_local(c, "(for step)");
            compile_exp(c, s->u.for_.start, var);
            compile_exp(c, s->u.for_.end, lim);
            if (s->u.for_.step) compile_exp(c, s->u.for_.step, stp);
            else emit(c, OP_LOADK, stp, add_constant(c, make_number(1)), 0, s->line);
            c->freereg = stp + 1;

            int isneg = 0;
            if (s->u.for_.step) {
                Exp *sp = s->u.for_.step;
                if (sp->type == EXP_NUMBER && sp->u.number < 0) isneg = 1;
                else if (sp->type == EXP_UNOP && sp->u.unop.op == TK_MINUS
                         && sp->u.unop.operand->type == EXP_NUMBER
                         && sp->u.unop.operand->u.number > 0) isneg = 1;
            }

            int start = c->code_size;
            int cond = c->freereg;
            reserve_reg(c, cond);
            if (isneg) emit(c, OP_LE, cond, lim, var, s->line);
            else emit(c, OP_LE, cond, var, lim, s->line);
            int jx = emit_jump(c, OP_TEST, cond, s->line);

            enter_scope(c);
            LoopCtx ctx; push_loop(c, &ctx);
            compile_block(c, s->u.for_.body);
            patch_breaks(c); pop_loop(c);
            leave_scope(c);

            emit(c, OP_ADD, var, var, stp, s->line);
            emit(c, OP_JMP, 0, start - (c->code_size + 1), 0, s->line);
            patch_jump(c, jx);
            break;
        }

        case STMT_RETURN: {
            int tmp = c->freereg;
            if (s->u.return_value) compile_exp(c, s->u.return_value, tmp);
            else emit(c, OP_LOADNIL, tmp, 0, 0, s->line);
            emit(c, OP_RETURN, tmp, 0, 0, s->line);
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

            int slot = find_local(c, s->u.func.name);
            if (slot >= 0) {
                emit(c, OP_CLOSURE, slot, idx, 0, s->line);
            } else {
                int uv = resolve_upval(c, s->u.func.name);
                int tmp = c->freereg;
                emit(c, OP_CLOSURE, tmp, idx, 0, s->line);
                if (uv >= 0) emit(c, OP_SETUPVAL, tmp, uv, 0, s->line);
                else emit(c, OP_SETGLOBAL, tmp, add_constant(c, make_string(s->u.func.name)), 0, s->line);
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
    compile_block(&c, body);

    emit(&c, OP_LOADNIL, c.freereg, 0, 0, 0);
    emit(&c, OP_RETURN, c.freereg, 0, 0, 0);

    Proto *p = (Proto *)calloc(1, sizeof(Proto));
    p->code = c.code;
    p->code_size = c.code_size;
    p->constants = c.constants;
    p->nconstants = c.nconstants;
    p->local_names = (char **)calloc(c.nlocals, sizeof(char *));
    for (int i = 0; i < c.nlocals; i++) p->local_names[i] = c.locals[i].name;
    p->nlocals = c.nlocals;
    p->nparams = nparams;
    p->nregs = c.maxregs > 0 ? c.maxregs : 1;
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
