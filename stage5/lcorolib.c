/*
 * lcorolib.c - 协程库（coroutine.create / resume / yield / status）
 * 官方对照: lua-5.1.5/src/lcorolib.c
 * 实现阶段: 阶段 5
 *
 * 协程是协作式线程：多个执行流共享一个 C 线程，手动切换。
 *
 * yield/resume 机制（迭代式 VM + yield flag）：
 *   yield 时设置 co->L->yield_requested=1, yield_value=val，返回。
 *   VM 主循环检测到 yield_requested，保存 yield_reg，返回 yield_value，帧栈原样保留。
 *   resume 时调用 resume_function：把 resume_value 写入 yield_reg 槽，继续 VM 主循环。
 *
 * 每个协程有独立的 lua_State（帧栈/寄存器），但共享全局环境。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobject.h"
#include "lauxlib.h"
#include "lvm.h"
#include "ltable.h"
#include "lgc.h"

/* 主线程的 lua_State（由 main.c 设置） */
extern lua_State *g_main_L;

/*
 * coroutine.create(f) — 创建协程，返回 thread 对象。
 *
 * 创建独立的 lua_State（共享主线程的全局环境），
 * 包装成 lua_Thread，状态为 CO_SUSPENDED。
 */
static lua_Value builtin_coroutine_create(lua_Value *args, int nargs) {
    if (nargs < 1 || args[0].type != LUA_TFUNCTION) {
        fprintf(stderr, "coroutine.create: 参数必须是函数\n");
        exit(1);
    }

    lua_Thread *th = (lua_Thread *)calloc(1, sizeof(lua_Thread));
    /* 获取主 lua_State 用于共享全局环境 */
    lua_State *main_L = g_current_thread ? g_current_thread->L : g_main_L;
    gc_register(main_L, &th->gc, LUA_TTHREAD);

    th->L = state_create();
    th->L->globals = main_L->globals;  /* 共享全局环境 */
    th->fn = args[0].v.fn;
    th->status = CO_SUSPENDED;
    th->first_time = 1;
    th->caller = NULL;
    th->yield_value = make_nil();
    th->resume_value = make_nil();
    return make_thread(th);
}

/*
 * coroutine.resume(co, ...) — 启动/恢复协程。
 *
 * 首次启动：call_function(co->L, co->fn, args) 运行协程。
 * 恢复：    resume_function(co->L, resume_value) 从 yield 点继续。
 *
 * 返回值是 yield 值或函数返回值。
 * 若协程 yield 了（co->L->yield_requested），状态设为 SUSPENDED。
 * 若协程正常返回，状态设为 DEAD。
 */
static lua_Value builtin_coroutine_resume(lua_Value *args, int nargs) {
    if (nargs < 1 || args[0].type != LUA_TTHREAD) {
        fprintf(stderr, "coroutine.resume: 参数必须是 thread\n");
        exit(1);
    }

    lua_Thread *co = args[0].v.th;
    if (co->status == CO_DEAD) {
        fprintf(stderr, "coroutine.resume: 无法 resume 已结束的协程\n");
        exit(1);
    }

    lua_Thread *caller = g_current_thread;
    co->caller = caller;
    g_current_thread = co;
    co->status = CO_RUNNING;

    /* 传递 resume 参数 */
    lua_Value resume_arg = (nargs >= 2) ? args[1] : make_nil();
    co->resume_value = resume_arg;

    lua_Value result;
    if (co->first_time) {
        /* 首次启动：调用协程函数 */
        co->first_time = 0;
        result = call_function(co->L, co->fn,
                               nargs >= 2 ? &args[1] : NULL,
                               nargs >= 2 ? nargs - 1 : 0);
    } else {
        /* 恢复：从 yield 点继续执行 */
        result = resume_function(co->L, resume_arg);
    }

    g_current_thread = caller;

    /* 判断协程是 yield 了还是正常结束 */
    if (co->L->yield_requested) {
        /* yield：vm_execute 因 yield 返回，帧栈保留 */
        co->L->yield_requested = 0;  /* 清除标志 */
        co->status = CO_SUSPENDED;
        return co->L->yield_value;
    } else {
        /* 正常返回：协程结束 */
        co->status = CO_DEAD;
        return result;
    }
}

/*
 * coroutine.yield(...) — 挂起当前协程，返回值给 resume。
 *
 * 设置 yield_requested=1 和 yield_value，然后返回。
 * VM 主循环（vm_execute）检测到 yield_requested 后：
 *   保存 yield_reg（当前 OP_CALL 的 A 寄存器），立即返回 yield_value，不弹帧。
 * 下次 resume 时，resume_function 把 resume_value 写入 yield_reg 槽，继续执行。
 */
static lua_Value builtin_coroutine_yield(lua_Value *args, int nargs) {
    lua_Thread *co = g_current_thread;
    if (!co) {
        fprintf(stderr, "coroutine.yield: 不在协程中\n");
        exit(1);
    }

    /* 设置 yield 标志和 yield 值 */
    co->L->yield_requested = 1;
    co->L->yield_value = (nargs >= 1) ? args[0] : make_nil();
    co->yield_value = co->L->yield_value;  /* 备份 */

    /* 返回值无关紧要——vm_execute 会返回 yield_value */
    return make_nil();
}

/*
 * coroutine.status(co) — 返回协程状态字符串。
 *
 * "suspended" — 已创建或已 yield
 * "running"   — 正在执行
 * "dead"      — 已结束
 * "normal"    — 已 resume 别的协程
 */
static lua_Value builtin_coroutine_status(lua_Value *args, int nargs) {
    if (nargs < 1 || args[0].type != LUA_TTHREAD)
        return make_string("dead");

    lua_Thread *co = args[0].v.th;
    switch (co->status) {
        case CO_SUSPENDED: return make_string("suspended");
        case CO_RUNNING:   return make_string("running");
        case CO_DEAD:      return make_string("dead");
        case CO_NORMAL:    return make_string("normal");
        default:           return make_string("dead");
    }
}

/*
 * open_corolib — 注册协程库。
 *
 * 创建全局 "coroutine" table，放入 create/resume/yield/status。
 */
void open_corolib(Env *env) {
    Table *lib = table_create();
    table_set(lib, make_string("create"), make_function(make_builtin(builtin_coroutine_create, "create")));
    table_set(lib, make_string("resume"), make_function(make_builtin(builtin_coroutine_resume, "resume")));
    table_set(lib, make_string("yield"),  make_function(make_builtin(builtin_coroutine_yield,  "yield")));
    table_set(lib, make_string("status"), make_function(make_builtin(builtin_coroutine_status, "status")));
    env_define(env, "coroutine", make_table(lib));
}
