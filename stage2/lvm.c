/*
 * lvm.c - VM 主循环 luaV_execute（解释执行栈式字节码、元方法调用）
 * 官方对照: lua-5.1.5/src/lvm.c
 * 实现阶段: 阶段 2+
 *
 * 阶段 2/3：栈式字节码 VM。
 *
 * 核心是 call_function 里的 while + switch 主循环：
 *   逐条取指令，根据 OpCode 执行对应操作。
 *   值栈用于表达式中间结果，调用帧的 locals 数组存局部变量。
 *
 * 对比阶段 4（寄存器式）：
 *   栈式：do_arith 从栈 pop 两个值，计算后 push 结果。
 *   寄存器式：OP_ADD A B C → R[A] = R[B] + R[C]，一步完成。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lvm.h"
/* ltable.h / ltm.h 已移除（阶段 2 不支持 table） */

/* ─── 环境操作（保留，用于全局变量） ─── */

Env *env_create(Env *parent) {
    Env *env = (Env *)calloc(1, sizeof(Env));
    env->parent = parent;
    return env;
}

void env_define(Env *env, const char *name, lua_Value value) {
    Binding *b = (Binding *)malloc(sizeof(Binding));
    b->name = strdup(name);
    b->value = value;
    b->next = env->bindings;
    env->bindings = b;
}

lua_Value *env_lookup(Env *env, const char *name) {
    for (Env *e = env; e != NULL; e = e->parent) {
        for (Binding *b = e->bindings; b != NULL; b = b->next) {
            if (strcmp(b->name, name) == 0)
                return &b->value;
        }
    }
    return NULL;
}

void env_assign(Env *env, const char *name, lua_Value value) {
    lua_Value *slot = env_lookup(env, name);
    if (slot) *slot = value;
    else env_define(env, name, value);
}

/* ─── lua_State ─── */

lua_State *state_create(void) {
    lua_State *L = (lua_State *)calloc(1, sizeof(lua_State));
    L->stack_cap = 256;
    L->stack = (lua_Value *)malloc(L->stack_cap * sizeof(lua_Value));
    L->top = 0;
    L->frame_cap = 64;
    L->frames = (CallFrame *)malloc(L->frame_cap * sizeof(CallFrame));
    L->frame_count = 0;
    L->globals = env_create(NULL);
    L->open_upvals = NULL;
    return L;
}

void state_free(lua_State *L) {
    free(L->stack);
    free(L->frames);
    free(L);
}

/* ─── 栈操作 ─── */

static void push(lua_State *L, lua_Value v) {
    if (L->top >= L->stack_cap) {
        L->stack_cap *= 2;
        L->stack = (lua_Value *)realloc(L->stack, L->stack_cap * sizeof(lua_Value));
    }
    L->stack[L->top++] = v;
}

static lua_Value pop(lua_State *L) {
    return L->stack[--L->top];
}

/* ─── Upvalue 管理（引用化：open/closed 状态机） ─── */

/*
 * find_or_create_open_upval — 查找或创建指向 slot 的 open UpVal。
 *
 * 如果已有指向该 slot 的 UpVal，复用之（多个闭包共享同一 upvalue）。
 * 否则创建新 UpVal，加入链表。
 *
 * 这是 Lua 闭包的关键：同一外层变量被多个闭包捕获时共享同一个 UpVal，
 * 一个闭包修改它，其他闭包也能看到。
 */
static UpVal *find_or_create_open_upval(lua_State *L, lua_Value *slot) {
    for (UpVal *uv = L->open_upvals; uv != NULL; uv = uv->next) {
        if (uv->ptr == slot)
            return uv;  /* 已存在，复用 */
    }
    /* 创建新 open UpVal，加入链表 */
    UpVal *uv = (UpVal *)malloc(sizeof(UpVal));
    uv->ptr = slot;      /* open：指向栈槽位 */
    uv->next = L->open_upvals;
    L->open_upvals = uv;
    return uv;
}

/*
 * close_upvals — 关闭所有 ptr 指向 [base, base+n) 范围的 open UpVal。
 *
 * 函数返回时调用，base = frame->locals, n = p->nlocals。
 * 把指向该帧 locals 的 UpVal 从 open 切换到 closed：
 *   把 *ptr 的值复制到 value，ptr 改为 &value。
 *
 * 注意：不能用指针大小比较来判断 UpVal 是否属于某帧，
 * 因为各帧的 locals 是独立 calloc 的，地址无序。
 * 必须检查 ptr 是否在 [base, base+n) 范围内。
 */
static void close_upvals(lua_State *L, lua_Value *base, int n) {
    UpVal **pp = &L->open_upvals;
    while (*pp != NULL) {
        UpVal *uv = *pp;
        if (uv->ptr >= base && uv->ptr < base + n) {
            uv->value = *uv->ptr;   /* 复制值 */
            uv->ptr = &uv->value;   /* 改为指向自身 value */
            *pp = uv->next;         /* 从链表移除 */
            uv->next = NULL;
        } else {
            pp = &uv->next;
        }
    }
}

/* ─── 闭包创建 ─── */

/*
 * create_closure — 根据 Proto 创建闭包，捕获 upvalue 引用。
 *
 * 遍历 Proto 的 upvalue 描述：
 *   is_local=1 → 从当前帧的 locals 捕获：创建/复用 open UpVal
 *   is_local=0 → 从当前帧闭包的 upvals 捕获：直接复用父 UpVal
 *
 * 关键区别（对比阶段 2）：
 *   阶段 2 捕获值拷贝（upvals 是 lua_Value[]）
 *   阶段 3 捕获引用（upvals 是 UpVal*[]），外层修改后闭包内能看到
 */
static Function *create_closure(Proto *proto, CallFrame *frame, lua_State *L) {
    Function *fn = (Function *)calloc(1, sizeof(Function));
    fn->is_builtin = 0;
    fn->u.user.proto = proto;
    fn->u.user.nupvals = proto->nupvals;
    if (proto->nupvals > 0) {
        fn->u.user.upvals = (UpVal **)malloc(proto->nupvals * sizeof(UpVal *));
        for (int i = 0; i < proto->nupvals; i++) {
            if (proto->upvals[i].is_local)
                /* 指向外层局部变量：创建/复用 open UpVal */
                fn->u.user.upvals[i] = find_or_create_open_upval(
                    L, &frame->locals[proto->upvals[i].index]);
            else
                /* 隔层捕获：直接复用父闭包的 UpVal */
                fn->u.user.upvals[i] = frame->fn->u.user.upvals[proto->upvals[i].index];
        }
    }
    return fn;
}

/* ─── 二元/一元运算（栈式：从栈 pop，结果 push） ─── */

static void do_arith(lua_State *L, OpCode op, int line) {
    lua_Value b = pop(L);
    lua_Value a = pop(L);
    if (a.type != LUA_TNUMBER || b.type != LUA_TNUMBER) {
        fprintf(stderr, "运行时错误（第 %d 行）：算术运算需要数字\n", line);
        exit(1);
    }
    double x = a.v.n, y = b.v.n, r;
    switch (op) {
        case OP_ADD: r = x + y; break;
        case OP_SUB: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_DIV: r = x / y; break;
        case OP_MOD: r = x - floor(x / y) * y; break;
        case OP_POW: r = pow(x, y); break;
        default:     r = 0;
    }
    push(L, make_number(r));
}

static void do_concat(lua_State *L, int line) {
    lua_Value b = pop(L);
    lua_Value a = pop(L);
    char buf_a[64], buf_b[64];
    const char *sa, *sb;
    if (a.type == LUA_TSTRING) sa = a.v.s;
    else if (a.type == LUA_TNUMBER) { snprintf(buf_a, sizeof(buf_a), "%g", a.v.n); sa = buf_a; }
    else { fprintf(stderr, "运行时错误（第 %d 行）：.. 需要字符串或数字\n", line); exit(1); }
    if (b.type == LUA_TSTRING) sb = b.v.s;
    else if (b.type == LUA_TNUMBER) { snprintf(buf_b, sizeof(buf_b), "%g", b.v.n); sb = buf_b; }
    else { fprintf(stderr, "运行时错误（第 %d 行）：.. 需要字符串或数字\n", line); exit(1); }
    char *result = (char *)malloc(strlen(sa) + strlen(sb) + 1);
    strcpy(result, sa);
    strcat(result, sb);
    push(L, make_string_owned(result));
}

static void do_compare(lua_State *L, OpCode op, int line) {
    lua_Value b = pop(L);
    lua_Value a = pop(L);
    int result;
    if (op == OP_EQ || op == OP_NE) {
        result = value_equals(a, b);
        push(L, make_boolean(op == OP_EQ ? result : !result));
        return;
    }
    if (a.type == LUA_TNUMBER && b.type == LUA_TNUMBER) {
        double x = a.v.n, y = b.v.n;
        switch (op) {
            case OP_LT: result = x <  y; break;
            case OP_GT: result = x >  y; break;
            case OP_LE: result = x <= y; break;
            case OP_GE: result = x >= y; break;
            default:    result = 0;
        }
    } else if (a.type == LUA_TSTRING && b.type == LUA_TSTRING) {
        int cmp = strcmp(a.v.s, b.v.s);
        switch (op) {
            case OP_LT: result = cmp <  0; break;
            case OP_GT: result = cmp >  0; break;
            case OP_LE: result = cmp <= 0; break;
            case OP_GE: result = cmp >= 0; break;
            default:    result = 0;
        }
    } else {
        fprintf(stderr, "运行时错误（第 %d 行）：比较运算需要相同类型\n", line);
        exit(1);
    }
    push(L, make_boolean(result));
}

/* ─── table 访问已移除（阶段 2 不支持 table） ─── */

/* ─── VM 主循环 ─── */

lua_Value call_function(lua_State *L, Function *fn, lua_Value *args, int nargs) {
    /* 内置函数：直接调用 C 函数 */
    if (fn->is_builtin)
        return fn->u.builtin(args, nargs);

    Proto *p = fn->u.user.proto;

    /* 确保帧栈有空间 */
    if (L->frame_count >= L->frame_cap) {
        L->frame_cap *= 2;
        L->frames = (CallFrame *)realloc(L->frames, L->frame_cap * sizeof(CallFrame));
    }

    /* 创建调用帧 */
    int frame_idx = L->frame_count;
    CallFrame *frame = &L->frames[L->frame_count++];
    frame->fn = fn;
    frame->ip = 0;
    frame->locals = (lua_Value *)calloc(p->nlocals > 0 ? p->nlocals : 1, sizeof(lua_Value));

    /* 绑定参数到 locals[0..nparams-1] */
    for (int i = 0; i < p->nparams; i++)
        frame->locals[i] = (i < nargs) ? args[i] : make_nil();

    /* 主循环：逐条执行指令 */
    lua_Value retval = make_nil();
    while (frame->ip < p->code_size) {
        Instruction inst = p->code[frame->ip++];
        int line = inst.line;

        switch (inst.op) {
            /* 值压栈 */
            case OP_NIL:    push(L, make_nil()); break;
            case OP_TRUE:   push(L, make_boolean(1)); break;
            case OP_FALSE:  push(L, make_boolean(0)); break;
            case OP_NUMBER: push(L, p->constants[inst.arg]); break;
            case OP_STRING: push(L, p->constants[inst.arg]); break;

            /* 局部变量 */
            case OP_GETLOCAL: push(L, frame->locals[inst.arg]); break;
            case OP_SETLOCAL: frame->locals[inst.arg] = pop(L); break;

            /* upvalue（引用化：通过 UpVal->ptr 间接访问） */
            case OP_GETUPVAL: push(L, *fn->u.user.upvals[inst.arg]->ptr); break;
            case OP_SETUPVAL: *fn->u.user.upvals[inst.arg]->ptr = pop(L); break;

            /* 全局变量 */
            case OP_GETGLOBAL: {
                const char *name = p->constants[inst.arg].v.s;
                lua_Value *slot = env_lookup(L->globals, name);
                push(L, slot ? *slot : make_nil());
                break;
            }
            case OP_SETGLOBAL:
                env_assign(L->globals, p->constants[inst.arg].v.s, pop(L));
                break;

            /* 算术运算 */
            case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_DIV: case OP_MOD: case OP_POW:
                do_arith(L, inst.op, line);
                break;

            /* 字符串连接 */
            case OP_CONCAT:
                do_concat(L, line);
                break;

            /* 比较运算 */
            case OP_EQ: case OP_NE: case OP_LT:
            case OP_GT: case OP_LE: case OP_GE:
                do_compare(L, inst.op, line);
                break;

            /* 一元运算 */
            case OP_NOT: {
                lua_Value v = pop(L);
                push(L, make_boolean(!is_truthy(v)));
                break;
            }
            case OP_NEG: {
                lua_Value v = pop(L);
                if (v.type != LUA_TNUMBER) {
                    fprintf(stderr, "运行时错误（第 %d 行）：一元负号需要数字\n", line);
                    exit(1);
                }
                push(L, make_number(-v.v.n));
                break;
            }
            case OP_LEN: {
                lua_Value v = pop(L);
                if (v.type == LUA_TSTRING)
                    push(L, make_number((double)strlen(v.v.s)));
                else {
                    fprintf(stderr, "运行时错误（第 %d 行）：# 需要字符串（阶段 2 不支持 table）\n", line);
                    exit(1);
                }
                break;
            }

            /* 控制流（不弹出栈顶，只查看；调用方按需 POP） */
            case OP_JUMP:  frame->ip = inst.arg; break;
            case OP_JUMPF: if (!is_truthy(L->stack[L->top - 1])) frame->ip = inst.arg; break;
            case OP_JUMPT: if (is_truthy(L->stack[L->top - 1])) frame->ip = inst.arg; break;
            case OP_POP:   pop(L); break;

            /* 函数调用 */
            case OP_CALL: {
                int nargs_call = inst.arg;
                lua_Value *call_args = &L->stack[L->top - nargs_call];
                lua_Value fn_val = L->stack[L->top - nargs_call - 1];
                L->top -= nargs_call + 1;
                if (fn_val.type != LUA_TFUNCTION) {
                    fprintf(stderr, "运行时错误（第 %d 行）：试图调用非函数值\n", line);
                    exit(1);
                }
                lua_Value result = call_function(L, fn_val.v.fn, call_args, nargs_call);
                /* frame 可能因 realloc 失效，重新获取 */
                frame = &L->frames[frame_idx];
                push(L, result);
                break;
            }

            /* 返回 */
            case OP_RETURN:
                retval = pop(L);
                goto done;

            /* 创建闭包 */
            case OP_CLOSURE: {
                Proto *sub = p->subprotos[inst.arg];
                Function *closure = create_closure(sub, frame, L);
                push(L, make_function(closure));
                break;
            }

            /* Table 操作已移除（阶段 2 不支持 table） */

            default:
                fprintf(stderr, "运行时错误（第 %d 行）：未知指令 %s\n", line, op_name(inst.op));
                exit(1);
        }
    }

done:
    /* frame 可能因嵌套调用导致 realloc 失效，重新获取 */
    frame = &L->frames[frame_idx];

    /* 关闭指向本帧 locals 的所有 open UpVal（闭包语义：外层返回后值固化） */
    close_upvals(L, frame->locals, p->nlocals);
    free(frame->locals);
    L->frame_count--;
    return retval;
}

/* ─── 执行主 chunk ─── */

lua_Value execute_main(lua_State *L, Proto *main_proto) {
    /* 把 main_proto 包装成闭包 */
    Function fn;
    memset(&fn, 0, sizeof(fn));
    fn.is_builtin = 0;
    fn.u.user.proto = main_proto;
    fn.u.user.upvals = NULL;
    fn.u.user.nupvals = 0;
    return call_function(L, &fn, NULL, 0);
}
