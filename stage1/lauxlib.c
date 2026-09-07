/*
 * lauxlib.c - 辅助库实现（luaL_checknumber / luaL_register / luaL_error 等）
 * 官方对照: lua-5.1.5/src/lauxlib.c
 * 实现阶段: 阶段 1+
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lauxlib.h"

Function *make_builtin(BuiltinFn fn, const char *name) {
    Function *f = (Function *)calloc(1, sizeof(Function));
    f->is_builtin = 1;
    f->name = strdup(name);
    f->u.builtin = fn;
    return f;
}

void register_builtin(Env *env, const char *name, BuiltinFn fn) {
    Function *f = make_builtin(fn, name);
    env_define(env, name, make_function(f));
}
