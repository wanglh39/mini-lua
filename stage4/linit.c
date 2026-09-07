/*
 * linit.c - 库注册入口（luaL_openlibs 一次性打开所有标准库）
 * 官方对照: lua-5.1.5/src/linit.c
 * 实现阶段: 阶段 1+
 *
 * 把所有标准库的注册函数集中调用。
 * 阶段 1 只有基础库；阶段 3+ 加入 string 库、table 库、math 库等。
 */

#include "lauxlib.h"

/* 基础库注册函数（定义在 lbaselib.c） */
extern void open_baselib(Env *env);
/* table 库注册函数（定义在 ltablib.c） */
extern void open_tablib(Env *env);

void luaL_openlibs(Env *env) {
    open_baselib(env);
    open_tablib(env);
    /* 阶段 4+ 在这里追加：
     *   open_stringlib(env);
     *   open_mathlib(env);
     */
}
