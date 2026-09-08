/*
 * lbaselib.c - 基础库（print / type / pairs / ipairs / assert / error / pcall）
 * 官方对照: lua-5.1.5/src/lbaselib.c
 * 实现阶段: 阶段 1+
 *
 * 阶段 1 实现的内置函数：
 *   print(...)     — 打印所有参数（tab 分隔），换行结尾
 *   type(v)        — 返回类型名字符串
 *   tostring(v)    — 转字符串
 *   tonumber(v)    — 转数字
 *   assert(v, msg) — 断言，假则报错
 *   error(msg)     — 抛出错误（阶段 1 简化：直接 exit）
 *
 * 阶段 3+ 会加入 pairs / ipairs / pcall / select 等（需要 table 支持）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobject.h"
#include "lauxlib.h"
#include "ltable.h"

/*
 * print — Lua 最常用的函数。
 *
 * 多个参数用 tab 分隔（不是空格），最后换行。
 * 这是 Lua 的标准行为，和 Python 的 print（空格分隔）不同。
 */
static lua_Value builtin_print(lua_Value *args, int nargs) {
    for (int i = 0; i < nargs; i++) {
        if (i > 0) printf("\t");
        print_value(args[i]);
    }
    printf("\n");
    return make_nil();
}

/* type — 返回值的类型名 */
static lua_Value builtin_type(lua_Value *args, int nargs) {
    if (nargs < 1) return make_nil();
    return make_string(type_name(args[0].type));
}

/*
 * tostring — 把值转成字符串。
 *
 * 数字/布尔/nil 直接转，字符串原样返回，函数输出 function: 0x地址。
 */
static lua_Value builtin_tostring(lua_Value *args, int nargs) {
    if (nargs < 1) return make_string("nil");
    lua_Value v = args[0];
    switch (v.type) {
        case LUA_TNIL:     return make_string("nil");
        case LUA_TBOOLEAN: return make_string(v.v.b ? "true" : "false");
        case LUA_TSTRING:  return make_string(v.v.s);
        case LUA_TNUMBER: {
            char buf[64];
            if (v.v.n == (double)(long)v.v.n)
                snprintf(buf, sizeof(buf), "%ld", (long)v.v.n);
            else
                snprintf(buf, sizeof(buf), "%g", v.v.n);
            return make_string(buf);
        }
        case LUA_TFUNCTION: {
            char buf[64];
            snprintf(buf, sizeof(buf), "function: 0x%p", (void *)v.v.fn);
            return make_string(buf);
        }
        case LUA_TTHREAD: {
            char buf[64];
            snprintf(buf, sizeof(buf), "thread: 0x%p", (void *)v.v.th);
            return make_string(buf);
        }
        default:
            return make_string("unknown");
    }
}

/* tonumber — 把字符串转数字，失败返回 nil */
static lua_Value builtin_tonumber(lua_Value *args, int nargs) {
    if (nargs < 1) return make_nil();
    lua_Value v = args[0];
    if (v.type == LUA_TNUMBER) return v;
    if (v.type == LUA_TSTRING) {
        char *end;
        double n = strtod(v.v.s, &end);
        if (end == v.v.s) return make_nil();  /* 解析失败 */
        return make_number(n);
    }
    return make_nil();
}

/* assert — 断言，假则报错退出 */
static lua_Value builtin_assert(lua_Value *args, int nargs) {
    if (nargs < 1 || !is_truthy(args[0])) {
        if (nargs >= 2 && args[1].type == LUA_TSTRING)
            fprintf(stderr, "assertion failed: %s\n", args[1].v.s);
        else
            fprintf(stderr, "assertion failed!\n");
        exit(1);
    }
    return args[0];
}

/* error — 抛出错误（阶段 1 简化：直接打印并退出） */
static lua_Value builtin_error(lua_Value *args, int nargs) {
    if (nargs >= 1 && args[0].type == LUA_TSTRING)
        fprintf(stderr, "错误: %s\n", args[0].v.s);
    else
        fprintf(stderr, "错误\n");
    exit(1);
}

/*
 * setmetatable(t, mt) — 设置 table 的元表，返回 t。
 *
 * 元表是 Lua OOP 的基础：通过 __index 实现继承，__newindex 实现代理等。
 */
static lua_Value builtin_setmetatable(lua_Value *args, int nargs) {
    if (nargs < 2 || args[0].type != LUA_TTABLE) {
        fprintf(stderr, "setmetatable: 第一个参数必须是 table\n");
        exit(1);
    }
    if (args[1].type != LUA_TTABLE && args[1].type != LUA_TNIL) {
        fprintf(stderr, "setmetatable: 第二个参数必须是 table 或 nil\n");
        exit(1);
    }
    args[0].v.t->metatable = (args[1].type == LUA_TTABLE) ? args[1].v.t : NULL;
    return args[0];
}

/* getmetatable(t) — 返回 table 的元表，无元表则返回 nil */
static lua_Value builtin_getmetatable(lua_Value *args, int nargs) {
    if (nargs < 1 || args[0].type != LUA_TTABLE)
        return make_nil();
    Table *mt = args[0].v.t->metatable;
    return mt ? make_table(mt) : make_nil();
}

/*
 * rawget(t, k) — 不经过元方法的 table 访问。
 * 用于在 __index 元方法内部直接访问原 table（避免无限递归）。
 */
static lua_Value builtin_rawget(lua_Value *args, int nargs) {
    if (nargs < 2 || args[0].type != LUA_TTABLE)
        return make_nil();
    lua_Value *slot = table_get(args[0].v.t, args[1]);
    return slot ? *slot : make_nil();
}

/* rawset(t, k, v) — 不经过元方法的 table 赋值 */
static lua_Value builtin_rawset(lua_Value *args, int nargs) {
    if (nargs < 3 || args[0].type != LUA_TTABLE)
        return make_nil();
    table_set(args[0].v.t, args[1], args[2]);
    return args[0];
}

/*
 * open_baselib — 注册基础库到环境。
 *
 * 由 linit.c 的 luaL_openlibs 调用。
 */
void open_baselib(Env *env) {
    register_builtin(env, "print",        builtin_print);
    register_builtin(env, "type",         builtin_type);
    register_builtin(env, "tostring",     builtin_tostring);
    register_builtin(env, "tonumber",     builtin_tonumber);
    register_builtin(env, "assert",       builtin_assert);
    register_builtin(env, "error",        builtin_error);
    register_builtin(env, "setmetatable", builtin_setmetatable);
    register_builtin(env, "getmetatable", builtin_getmetatable);
    register_builtin(env, "rawget",       builtin_rawget);
    register_builtin(env, "rawset",       builtin_rawset);
}
