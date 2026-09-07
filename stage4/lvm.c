/*
 * lvm.c - VM 主循环 luaV_execute（解释执行寄存器字节码、元方法调用）
 * 官方对照: lua-5.1.5/src/lvm.c
 * 实现阶段: 阶段 4+
 *
 * 阶段 4：寄存器式字节码 VM。
 *
 * 核心是 call_function 里的 while + switch 主循环：
 *   逐条取指令，根据 OpCode 执行对应操作。
 *   指令通过 A/B/C 三个操作数指定源和目标寄存器。
 *   无需 push/pop，直接读写 R[A]/R[B]/R[C]。
 *
 * 对比阶段 2/3（栈式）：
 *   栈式：do_arith 从栈 pop 两个值，计算后 push 结果。
 *   寄存器式：OP_ADD A B C → R[A] = R[B] + R[C]，一步完成。
 *   指令更少、访问更直接、更适合现代 CPU。
 *
 * 寄存器布局（每个调用帧独立）：
 *   R[0..nparams-1]       — 函数参数（调用时从调用方寄存器拷入）
 *   R[nparams..nlocals-1] — 局部变量
 *   R[nlocals..nregs-1]   — 表达式求值的临时寄存器
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lvm.h"
#include "ltable.h"
#include "ltm.h"

/* ─── 环境操作（全局变量，链表实现） ─── */

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
    L->frame_cap = 64;
    L->frames = (CallFrame *)malloc(L->frame_cap * sizeof(CallFrame));
    L->frame_count = 0;
    L->globals = env_create(NULL);
    L->open_upvals = NULL;
    return L;
}

void state_free(lua_State *L) {
    free(L->frames);
    free(L);
}

/* ─── Upvalue 管理（引用化：open/closed 状态机） ─── */

/*
 * find_or_create_open_upval — 查找或创建指向 slot 的 open UpVal。
 *
 * 如果已有指向该 slot 的 UpVal，复用之（多个闭包共享同一 upvalue）。
 * 否则创建新 UpVal，加入链表。
 *
 * slot 现在指向帧的 regs 槽位（&frame->regs[index]）。
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
    uv->ptr = slot;      /* open：指向寄存器槽位 */
    uv->next = L->open_upvals;
    L->open_upvals = uv;
    return uv;
}

/*
 * close_upvals — 关闭所有 ptr 指向 [base, base+n) 范围的 open UpVal。
 *
 * 函数返回时调用，base = frame->regs, n = p->nregs。
 * 把指向该帧 regs 的 UpVal 从 open 切换到 closed：
 *   把 *ptr 的值复制到 value，ptr 改为 &value。
 *
 * 注意：不能用指针大小比较来判断 UpVal 是否属于某帧，
 * 因为各帧的 regs 是独立 calloc 的，地址无序。
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
 *   is_local=1 → 从当前帧的 regs 捕获：创建/复用 open UpVal
 *   is_local=0 → 从当前帧闭包的 upvals 捕获：直接复用父 UpVal
 *
 * 关键：upvals 是 UpVal*[]（引用），外层修改后闭包内能看到。
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
                /* 指向外层寄存器变量：创建/复用 open UpVal */
                fn->u.user.upvals[i] = find_or_create_open_upval(
                    L, &frame->regs[proto->upvals[i].index]);
            else
                /* 隔层捕获：直接复用父闭包的 UpVal */
                fn->u.user.upvals[i] = frame->fn->u.user.upvals[proto->upvals[i].index];
        }
    }
    return fn;
}

/* ─── 二元/一元运算（寄存器式：接收值参数，返回结果值） ─── */

/*
 * do_arith — 算术运算，返回 R[B] op R[C] 的结果。
 *
 * 寄存器式改造：不再 pop/push，直接接收 a/b 两个值，返回结果。
 */
static lua_Value do_arith(OpCode op, lua_Value a, lua_Value b, int line) {
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
    return make_number(r);
}

/*
 * do_concat — 字符串连接，返回 R[B] .. R[C] 的结果。
 */
static lua_Value do_concat(lua_Value a, lua_Value b, int line) {
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
    return make_string_owned(result);
}

/*
 * do_compare — 比较运算，返回布尔值 (R[B] op R[C])。
 */
static lua_Value do_compare(OpCode op, lua_Value a, lua_Value b, int line) {
    int result;
    if (op == OP_EQ || op == OP_NE) {
        result = value_equals(a, b);
        return make_boolean(op == OP_EQ ? result : !result);
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
    return make_boolean(result);
}

/* ─── table 访问（带元方法） ─── */

/*
 * table_getindex — 递归查找 table[key]，支持 __index 元方法链。
 *
 * 查找顺序：
 *   1. 直接在 t 中查 key
 *   2. 若不存在，查 t 的 metatable.__index：
 *      - __index 是 table → 递归查 __index[key]（可能又走 __index）
 *      - __index 是 function → 调用 __index(t, key)
 *   3. 仍不存在 → nil
 */
static lua_Value table_getindex(lua_State *L, lua_Value tbl, lua_Value key, int line) {
    if (tbl.type != LUA_TTABLE) {
        fprintf(stderr, "运行时错误（第 %d 行）：索引访问需要 table\n", line);
        exit(1);
    }
    lua_Value *slot = table_get(tbl.v.t, key);
    if (slot)
        return *slot;

    /* 查 __index 元方法 */
    lua_Value *mm = get_metamethod(tbl.v.t, "__index");
    if (mm && mm->type == LUA_TTABLE)
        return table_getindex(L, *mm, key, line);  /* 递归 */
    if (mm && mm->type == LUA_TFUNCTION) {
        lua_Value args[2] = {tbl, key};
        return call_function(L, mm->v.fn, args, 2);
    }
    return make_nil();
}

/*
 * table_setindex — 带 __newindex 元方法的 table[key] = value。
 *
 * 若 key 已存在，直接更新。
 * 若 key 不存在且 metatable.__newindex 有定义：
 *   __newindex 是 table → 在 __newindex 上设 key=value
 *   __newindex 是 function → 调用 __newindex(table, key, value)
 */
static void table_setindex(lua_State *L, lua_Value tbl, lua_Value key, lua_Value value, int line) {
    if (tbl.type != LUA_TTABLE) {
        fprintf(stderr, "运行时错误（第 %d 行）：索引赋值需要 table\n", line);
        exit(1);
    }
    if (table_get(tbl.v.t, key) != NULL) {
        /* 键已存在，直接更新 */
        table_set(tbl.v.t, key, value);
    } else {
        /* 键不存在，查 __newindex */
        lua_Value *mm = get_metamethod(tbl.v.t, "__newindex");
        if (mm && mm->type == LUA_TTABLE) {
            table_set(mm->v.t, key, value);
        } else if (mm && mm->type == LUA_TFUNCTION) {
            lua_Value args[3] = {tbl, key, value};
            call_function(L, mm->v.fn, args, 3);
        } else {
            table_set(tbl.v.t, key, value);
        }
    }
}

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

    /* 创建调用帧，分配寄存器数组 */
    CallFrame *frame = &L->frames[L->frame_count++];
    frame->fn = fn;
    frame->ip = 0;
    int nregs = p->nregs > 0 ? p->nregs : 1;
    frame->regs = (lua_Value *)calloc(nregs, sizeof(lua_Value));

    /* 绑定参数到 R[0..nparams-1] */
    for (int i = 0; i < p->nparams; i++)
        frame->regs[i] = (i < nargs) ? args[i] : make_nil();

    /* 寄存器访问宏：R[i] = frame->regs[i] */
    lua_Value *R = frame->regs;

    /* 主循环：逐条执行指令 */
    lua_Value retval = make_nil();
    while (frame->ip < p->code_size) {
        Instruction inst = p->code[frame->ip++];
        int line = inst.line;
        int A = inst.A, B = inst.B, C = inst.C;

        switch (inst.op) {
            /* ─── 值加载到寄存器 ─── */
            case OP_LOADK:
                R[A] = p->constants[B];
                break;
            case OP_LOADNIL:
                R[A] = make_nil();
                break;
            case OP_LOADBOOL:
                R[A] = make_boolean(B != 0);
                break;

            /* ─── 寄存器复制 ─── */
            case OP_MOVE:
                R[A] = R[B];
                break;

            /* ─── upvalue 访问（通过 UpVal->ptr 间接访问） ─── */
            case OP_GETUPVAL:
                R[A] = *fn->u.user.upvals[B]->ptr;
                break;
            case OP_SETUPVAL:
                *fn->u.user.upvals[B]->ptr = R[A];
                break;

            /* ─── 全局变量访问 ─── */
            case OP_GETGLOBAL: {
                const char *name = p->constants[B].v.s;
                lua_Value *slot = env_lookup(L->globals, name);
                R[A] = slot ? *slot : make_nil();
                break;
            }
            case OP_SETGLOBAL:
                env_assign(L->globals, p->constants[B].v.s, R[A]);
                break;

            /* ─── Table 操作 ─── */
            case OP_NEWTABLE:
                R[A] = make_table(table_create());
                break;
            case OP_GETTABLE:
                /* R[A] = R[B][R[C]]，带 __index 元方法递归查找 */
                R[A] = table_getindex(L, R[B], R[C], line);
                break;
            case OP_SETTABLE:
                /* R[A][R[B]] = R[C]，带 __newindex 元方法 */
                table_setindex(L, R[A], R[B], R[C], line);
                break;
            case OP_SETTABLEK:
                /* R[A][K[B]] = R[C]，K[B] 是常量键（用于 table 构造的字符串键） */
                table_setindex(L, R[A], p->constants[B], R[C], line);
                break;

            /* ─── 二元算术: R[A] = R[B] op R[C] ─── */
            case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_DIV: case OP_MOD: case OP_POW:
                R[A] = do_arith(inst.op, R[B], R[C], line);
                break;

            /* ─── 比较运算: R[A] = (R[B] op R[C]) → 布尔结果 ─── */
            case OP_EQ: case OP_NE: case OP_LT:
            case OP_GT: case OP_LE: case OP_GE:
                R[A] = do_compare(inst.op, R[B], R[C], line);
                break;

            /* ─── 一元运算: R[A] = op R[B] ─── */
            case OP_NEG:
                if (R[B].type != LUA_TNUMBER) {
                    fprintf(stderr, "运行时错误（第 %d 行）：一元负号需要数字\n", line);
                    exit(1);
                }
                R[A] = make_number(-R[B].v.n);
                break;
            case OP_NOT:
                R[A] = make_boolean(!is_truthy(R[B]));
                break;
            case OP_LEN:
                if (R[B].type == LUA_TSTRING)
                    R[A] = make_number((double)strlen(R[B].v.s));
                else if (R[B].type == LUA_TTABLE)
                    R[A] = make_number((double)table_len(R[B].v.t));
                else {
                    fprintf(stderr, "运行时错误（第 %d 行）：# 需要字符串或 table\n", line);
                    exit(1);
                }
                break;

            /* ─── 字符串连接: R[A] = R[B] .. R[C] ─── */
            case OP_CONCAT:
                R[A] = do_concat(R[B], R[C], line);
                break;

            /* ─── 控制流 ─── */
            case OP_JMP:
                /* 相对跳转：pc += B */
                frame->ip += B;
                break;
            case OP_TEST:
                /* if not is_truthy(R[A]) then pc += B（相对跳转到 else/短路后） */
                if (!is_truthy(R[A])) frame->ip += B;
                break;
            case OP_TESTN:
                /* if is_truthy(R[A]) then pc += B（相对跳转到 or 短路后） */
                if (is_truthy(R[A])) frame->ip += B;
                break;

            /* ─── 函数调用 ─── */
            case OP_CALL: {
                /*
                 * R[A] = R[A](R[A+1..A+B])
                 * B = 参数个数
                 * 参数已经在 R[A+1..A+B] 中，直接传 &R[A+1] 给 call_function
                 * 返回值放回 R[A]
                 */
                lua_Value fn_val = R[A];
                if (fn_val.type != LUA_TFUNCTION) {
                    fprintf(stderr, "运行时错误（第 %d 行）：试图调用非函数值\n", line);
                    exit(1);
                }
                R[A] = call_function(L, fn_val.v.fn, &R[A + 1], B);
                break;
            }

            /* ─── 返回 ─── */
            case OP_RETURN:
                retval = R[A];
                goto done;

            /* ─── 创建闭包 ─── */
            case OP_CLOSURE: {
                /* R[A] = closure(Proto[B]) */
                Proto *sub = p->subprotos[B];
                Function *closure = create_closure(sub, frame, L);
                R[A] = make_function(closure);
                break;
            }

            default:
                fprintf(stderr, "运行时错误（第 %d 行）：未知指令 %s\n", line, op_name(inst.op));
                exit(1);
        }
    }

done:
    /* 关闭指向本帧 regs 的所有 open UpVal（闭包语义：外层返回后值固化） */
    close_upvals(L, frame->regs, nregs);
    free(frame->regs);
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
