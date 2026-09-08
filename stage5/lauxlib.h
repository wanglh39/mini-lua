/*
 * lauxlib.h - C 辅助库（luaL_checknumber 等便利工具）
 * 官方对照: lua-5.1.5/src/lauxlib.h
 * 实现阶段: 阶段 1+
 *
 * 阶段 1 简化：只有创建/注册内置函数的辅助，以及标准库打开入口。
 * 官方 lauxlib 有大量参数检查工具（luaL_checknumber 等），阶段 2+ 再加。
 */

#ifndef lauxlib_h
#define lauxlib_h

#include "lobject.h"
#include "lvm.h"

/* 创建内置函数对象（包装 C 函数指针） */
Function *make_builtin(BuiltinFn fn, const char *name);

/* 在环境中注册一个内置函数 */
void register_builtin(Env *env, const char *name, BuiltinFn fn);

/* 打开所有标准库（注册到环境 env） */
void luaL_openlibs(Env *env);

#endif /* lauxlib_h */
