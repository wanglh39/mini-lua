/*
 * lvm.c - 树遍历解释器实现（eval_exp / exec_stmt / call_function）
 * 官方对照: lua-5.1.5/src/lvm.c
 * 实现阶段: 阶段 1（树遍历解释器）
 *
 * 核心是两个互递归函数：
 *   eval_exp(env, e) → lua_Value    求值表达式
 *   exec_stmt(env, s) → Ctrl        执行语句（返回控制流状态）
 *
 * 对比阶段 2+（字节码 VM）：
 *   阶段 1：直接遍历 AST，变量在 Env 链表中，闭包 = {body, env}
 *   阶段 2+：先编译成字节码（Proto），再 VM 主循环执行，变量在 locals 数组
 *
 * 树遍历的优点：实现简单，无需编译器/字节码/寄存器分配。
 * 树遍历的缺点：每次执行都重新遍历 AST，性能远低于字节码 VM。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lvm.h"

/* ─── 辅助：在全局环境定义变量（赋值语义） ─── */

/*
 * env_set — 赋值语义：查找已有绑定并更新，找不到则在全局环境定义。
 *
 * Lua 赋值规则：name = value
 *   - 如果 name 是局部变量或 upvalue → 更新该绑定
 *   - 如果 name 不存在 → 在全局环境创建新绑定
 *
 * 对比 env_define：env_define 总是在当前层新建（local 声明用）。
 */
static void env_set(Env *env, const char *name, lua_Value value) {
    lua_Value *slot = env_lookup(env, name);
    if (slot) {
        *slot = value;
    } else {
        /* 找不到，在全局环境（env 链根）定义 */
        Env *global = env;
        while (global->parent) global = global->parent;
        env_define(global, name, value);
    }
}

/* ─── 二元算术运算 ─── */

static lua_Value do_arith(lua_Value a, lua_Value b, TokenType op, int line) {
    if (a.type != LUA_TNUMBER || b.type != LUA_TNUMBER) {
        fprintf(stderr, "运行时错误（第 %d 行）：算术运算需要数字\n", line);
        exit(1);
    }
    double x = a.v.n, y = b.v.n, r;
    switch (op) {
        case TK_PLUS:    r = x + y; break;
        case TK_MINUS:   r = x - y; break;
        case TK_STAR:    r = x * y; break;
        case TK_SLASH:   r = x / y; break;
        case TK_PERCENT: r = x - floor(x / y) * y; break;
        case TK_CARET:   r = pow(x, y); break;
        default:         r = 0;
    }
    return make_number(r);
}

/* ─── 字符串连接 ─── */

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

/* ─── 比较运算 ─── */

static lua_Value do_compare(lua_Value a, lua_Value b, TokenType op, int line) {
    int result;
    if (op == TK_EQ || op == TK_NE) {
        result = value_equals(a, b);
        return make_boolean(op == TK_EQ ? result : !result);
    }
    if (a.type == LUA_TNUMBER && b.type == LUA_TNUMBER) {
        double x = a.v.n, y = b.v.n;
        switch (op) {
            case TK_LT: result = x <  y; break;
            case TK_GT: result = x >  y; break;
            case TK_LE: result = x <= y; break;
            case TK_GE: result = x >= y; break;
            default:    result = 0;
        }
    } else if (a.type == LUA_TSTRING && b.type == LUA_TSTRING) {
        int cmp = strcmp(a.v.s, b.v.s);
        switch (op) {
            case TK_LT: result = cmp <  0; break;
            case TK_GT: result = cmp >  0; break;
            case TK_LE: result = cmp <= 0; break;
            case TK_GE: result = cmp >= 0; break;
            default:    result = 0;
        }
    } else {
        fprintf(stderr, "运行时错误（第 %d 行）：比较运算需要相同类型\n", line);
        exit(1);
    }
    return make_boolean(result);
}

/* ─── 表达式求值 ─── */

lua_Value eval_exp(Env *env, Exp *e) {
    switch (e->type) {
        case EXP_NIL:
            return make_nil();

        case EXP_BOOL:
            return make_boolean(e->u.boolean);

        case EXP_NUMBER:
            return make_number(e->u.number);

        case EXP_STRING:
            return make_string(e->u.string);

        case EXP_VAR: {
            lua_Value *slot = env_lookup(env, e->u.var);
            if (!slot) {
                fprintf(stderr, "运行时错误（第 %d 行）：变量 '%s' 未定义\n", e->line, e->u.var);
                exit(1);
            }
            return *slot;
        }

        case EXP_BINOP: {
            /* and/or 短路求值 */
            if (e->u.binop.op == TK_AND) {
                lua_Value a = eval_exp(env, e->u.binop.left);
                if (!is_truthy(a)) return a;
                return eval_exp(env, e->u.binop.right);
            }
            if (e->u.binop.op == TK_OR) {
                lua_Value a = eval_exp(env, e->u.binop.left);
                if (is_truthy(a)) return a;
                return eval_exp(env, e->u.binop.right);
            }

            /* 普通二元运算 */
            lua_Value a = eval_exp(env, e->u.binop.left);
            lua_Value b = eval_exp(env, e->u.binop.right);

            /* 算术运算 */
            if (e->u.binop.op == TK_PLUS || e->u.binop.op == TK_MINUS ||
                e->u.binop.op == TK_STAR  || e->u.binop.op == TK_SLASH ||
                e->u.binop.op == TK_PERCENT || e->u.binop.op == TK_CARET)
                return do_arith(a, b, e->u.binop.op, e->line);

            /* 字符串连接 */
            if (e->u.binop.op == TK_CONCAT)
                return do_concat(a, b, e->line);

            /* 比较运算 */
            return do_compare(a, b, e->u.binop.op, e->line);
        }

        case EXP_UNOP: {
            lua_Value v = eval_exp(env, e->u.unop.operand);
            switch (e->u.unop.op) {
                case TK_MINUS:
                    if (v.type != LUA_TNUMBER) {
                        fprintf(stderr, "运行时错误（第 %d 行）：一元负号需要数字\n", e->line);
                        exit(1);
                    }
                    return make_number(-v.v.n);
                case TK_NOT:
                    return make_boolean(!is_truthy(v));
                case TK_LEN:
                    if (v.type == LUA_TSTRING)
                        return make_number((double)strlen(v.v.s));
                    fprintf(stderr, "运行时错误（第 %d 行）：# 需要字符串\n", e->line);
                    exit(1);
                default:
                    fprintf(stderr, "未知一元运算符\n");
                    exit(1);
            }
        }

        case EXP_CALL: {
            /* 求值函数表达式 */
            lua_Value fn_val = eval_exp(env, e->u.call.fn);
            if (fn_val.type != LUA_TFUNCTION) {
                fprintf(stderr, "运行时错误（第 %d 行）：试图调用非函数值\n", e->line);
                exit(1);
            }
            /* 求值参数 */
            int nargs = 0;
            lua_Value args[64];  /* 简化：最多 64 个参数 */
            for (ExpList *l = e->u.call.args; l; l = l->tail) {
                if (nargs >= 64) {
                    fprintf(stderr, "运行时错误：参数过多\n");
                    exit(1);
                }
                args[nargs++] = eval_exp(env, l->head);
            }
            return call_function(fn_val.v.fn, args, nargs);
        }

        case EXP_FUNCTION: {
            /*
             * 创建闭包：捕获当前 env 作为定义时环境。
             *
             * 闭包 = {params, body, env}
             * 调用时以此 env 为 parent 创建新 env，绑定参数后执行 body。
             * 由于 env 是引用（非拷贝），外层变量修改后闭包内能看到。
             */
            Function *fn = (Function *)calloc(1, sizeof(Function));
            fn->is_builtin = 0;
            fn->u.user.params = e->u.func.params;
            fn->u.user.nparams = e->u.func.nparams;
            fn->u.user.body = e->u.func.body;
            fn->u.user.env = env;  /* 捕获定义时环境 */
            return make_function(fn);
        }

        case EXP_TABLE:
            fprintf(stderr, "编译错误（第 %d 行）：阶段 1 不支持 table 构造\n", e->line);
            exit(1);

        case EXP_INDEX:
            fprintf(stderr, "编译错误（第 %d 行）：阶段 1 不支持索引访问\n", e->line);
            exit(1);
    }
    return make_nil();  /* 不会到达 */
}

/* ─── 语句执行 ─── */

Ctrl exec_stmt(Env *env, Stmt *s) {
    switch (s->type) {
        case STMT_EXPR:
            eval_exp(env, s->u.expr);
            break;

        case STMT_LOCAL: {
            lua_Value value = eval_exp(env, s->u.local.value);
            env_define(env, s->u.local.name, value);
            break;
        }

        case STMT_ASSIGN: {
            lua_Value value = eval_exp(env, s->u.assign.value);
            env_set(env, s->u.assign.name, value);
            break;
        }

        case STMT_IF: {
            lua_Value cond = eval_exp(env, s->u.if_.cond);
            if (is_truthy(cond)) {
                Env *then_env = env_create(env);
                return exec_block(then_env, s->u.if_.then_branch);
            } else if (s->u.if_.else_branch) {
                Env *else_env = env_create(env);
                return exec_block(else_env, s->u.if_.else_branch);
            }
            break;
        }

        case STMT_WHILE: {
            while (1) {
                lua_Value cond = eval_exp(env, s->u.while_.cond);
                if (!is_truthy(cond)) break;
                /* 每次迭代创建新 env（块作用域） */
                Env *body_env = env_create(env);
                Ctrl result = exec_block(body_env, s->u.while_.body);
                if (result.control == CONTROL_RETURN) return result;
                if (result.control == CONTROL_BREAK) break;
            }
            break;
        }

        case STMT_FOR: {
            /*
             * for var = start, end[, step] do body end
             *
             * 创建 loop_env 存循环变量，每次迭代创建 body_env（parent = loop_env）。
             * 循环变量在 loop_env 中，body 中的局部变量在 body_env 中（每次迭代独立）。
             */
            Env *loop_env = env_create(env);

            double start = eval_exp(env, s->u.for_.start).v.n;
            double end = eval_exp(env, s->u.for_.end).v.n;
            double step = s->u.for_.step ? eval_exp(env, s->u.for_.step).v.n : 1.0;

            double var = start;
            while (step > 0 ? var <= end : var >= end) {
                /* 在 loop_env 中设置/更新循环变量 */
                lua_Value *slot = env_lookup(loop_env, s->u.for_.var);
                if (slot) *slot = make_number(var);
                else env_define(loop_env, s->u.for_.var, make_number(var));

                /* 执行循环体 */
                Env *body_env = env_create(loop_env);
                Ctrl result = exec_block(body_env, s->u.for_.body);
                if (result.control == CONTROL_RETURN) return result;
                if (result.control == CONTROL_BREAK) break;

                var += step;
            }
            break;
        }

        case STMT_RETURN: {
            Ctrl ctrl;
            ctrl.control = CONTROL_RETURN;
            if (s->u.return_value)
                ctrl.retval = eval_exp(env, s->u.return_value);
            else
                ctrl.retval = make_nil();
            return ctrl;
        }

        case STMT_BREAK:
            return (Ctrl){CONTROL_BREAK, make_nil()};

        case STMT_FUNCTION: {
            /*
             * function name(params) body end
             * 等价于 name = function(params) body end
             */
            Function *fn = (Function *)calloc(1, sizeof(Function));
            fn->is_builtin = 0;
            fn->u.user.params = s->u.func.params;
            fn->u.user.nparams = s->u.func.nparams;
            fn->u.user.body = s->u.func.body;
            fn->u.user.env = env;  /* 捕获定义时环境 */
            env_set(env, s->u.func.name, make_function(fn));
            break;
        }

        case STMT_INDEX_ASSIGN:
            fprintf(stderr, "编译错误（第 %d 行）：阶段 1 不支持索引赋值\n", s->line);
            exit(1);
    }
    return (Ctrl){CONTROL_NORMAL, make_nil()};
}

/* ─── 语句块执行 ─── */

Ctrl exec_block(Env *env, Stmt *block) {
    for (Stmt *s = block; s; s = s->next) {
        Ctrl result = exec_stmt(env, s);
        if (result.control != CONTROL_NORMAL)
            return result;  /* return 或 break：中断并传播 */
    }
    return (Ctrl){CONTROL_NORMAL, make_nil()};
}

/* ─── 函数调用 ─── */

lua_Value call_function(Function *fn, lua_Value *args, int nargs) {
    /* 内置函数：直接调用 C 函数 */
    if (fn->is_builtin)
        return fn->u.builtin(args, nargs);

    /*
     * 用户函数：创建新 env（parent = 闭包捕获的 env），绑定参数，执行 body。
     *
     * 闭包语义：fn->u.user.env 是定义时捕获的环境。
     *   调用时以此 env 为 parent 创建新 env，变量查找沿 parent 链向上，
     *   自然实现 upvalue 引用语义（外层修改后闭包内能看到）。
     */
    Env *call_env = env_create(fn->u.user.env);

    /* 绑定参数到 call_env */
    for (int i = 0; i < fn->u.user.nparams; i++) {
        lua_Value arg = (i < nargs) ? args[i] : make_nil();
        env_define(call_env, fn->u.user.params[i], arg);
    }

    /* 执行函数体 */
    Ctrl result = exec_block(call_env, fn->u.user.body);
    if (result.control == CONTROL_RETURN)
        return result.retval;
    return make_nil();  /* 无 return 语句，默认返回 nil */
}

/* ─── 执行主 chunk ─── */

void eval_program(Env *globals, Stmt *program) {
    exec_block(globals, program);
}