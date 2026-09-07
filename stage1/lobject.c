/*
 * lobject.c - 对象基本操作辅助（值构造 / 输出 / 真值判断 / 相等判断 / 环境操作）
 * 官方对照: lua-5.1.5/src/lobject.c
 * 实现阶段: 阶段 1（树遍历解释器）
 *
 * 阶段 1 无 op_name（无字节码指令），无 make_table（不支持 table）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobject.h"

/* ─── 值构造 ─── */

lua_Value make_nil(void) {
    lua_Value v;
    v.type = LUA_TNIL;
    return v;
}

lua_Value make_boolean(int b) {
    lua_Value v;
    v.type = LUA_TBOOLEAN;
    v.v.b = b ? 1 : 0;
    return v;
}

lua_Value make_number(double n) {
    lua_Value v;
    v.type = LUA_TNUMBER;
    v.v.n = n;
    return v;
}

lua_Value make_string(const char *s) {
    lua_Value v;
    v.type = LUA_TSTRING;
    v.v.s = strdup(s);  /* 复制一份，调用方可以放心释放原串 */
    return v;
}

lua_Value make_string_owned(char *s) {
    lua_Value v;
    v.type = LUA_TSTRING;
    v.v.s = s;  /* 直接接管，不再复制 */
    return v;
}

lua_Value make_function(Function *fn) {
    lua_Value v;
    v.type = LUA_TFUNCTION;
    v.v.fn = fn;
    return v;
}

/* ─── 值输出与判断 ─── */

const char *type_name(LuaType type) {
    switch (type) {
        case LUA_TNIL:      return "nil";
        case LUA_TBOOLEAN:  return "boolean";
        case LUA_TNUMBER:   return "number";
        case LUA_TSTRING:   return "string";
        case LUA_TFUNCTION: return "function";
        case LUA_TTABLE:    return "table";
        default:            return "unknown";
    }
}

/*
 * print_value — 值的文本输出。
 *
 * 数字：整数就输出整数形式（3 而非 3.0），小数输出小数。
 *   Lua 5.1 的数字是 double，但显示时整数不带小数点。
 * 字符串：直接输出内容（不加引号，print 的语义）。
 * 函数：输出 function 0x地址（调试用）。
 */
void print_value(lua_Value v) {
    switch (v.type) {
        case LUA_TNIL:
            printf("nil");
            break;
        case LUA_TBOOLEAN:
            printf(v.v.b ? "true" : "false");
            break;
        case LUA_TNUMBER:
            /* 整数显示为整数 */
            if (v.v.n == (double)(long)v.v.n)
                printf("%ld", (long)v.v.n);
            else
                printf("%g", v.v.n);
            break;
        case LUA_TSTRING:
            printf("%s", v.v.s);
            break;
        case LUA_TFUNCTION:
            printf("function: 0x%p", (void *)v.v.fn);
            break;
        case LUA_TTABLE:
            printf("table: 0x%p", (void *)v.v.fn);  /* 不会到达 */
            break;
    }
}

/*
 * is_truthy — Lua 的真值判断。
 *
 * Lua 中只有 nil 和 false 为假，其余（包括 0、空串）都为真。
 * 这和 C/Python 不同（C 的 0 为假，Python 的空串为假）。
 */
int is_truthy(lua_Value v) {
    if (v.type == LUA_TNIL) return 0;
    if (v.type == LUA_TBOOLEAN && v.v.b == 0) return 0;
    return 1;
}

/*
 * value_equals — 值的相等判断（== 运算）。
 *
 * 类型不同直接不等。
 * 数字按浮点比较，字符串按 strcmp，布尔按值，函数按指针。
 * nil == nil 为真。
 */
int value_equals(lua_Value a, lua_Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case LUA_TNIL:      return 1;
        case LUA_TBOOLEAN:  return a.v.b == b.v.b;
        case LUA_TNUMBER:   return a.v.n == b.v.n;
        case LUA_TSTRING:   return strcmp(a.v.s, b.v.s) == 0;
        case LUA_TFUNCTION: return a.v.fn == b.v.fn;
        default:            return 0;
    }
}

/* ─── 环境操作 ─── */

/*
 * env_create — 创建新环境，parent 指向外层。
 *
 * 全局环境：parent = NULL
 * 函数环境：parent = 闭包捕获的 env
 * 块作用域：parent = 外层 env
 */
Env *env_create(Env *parent) {
    Env *env = (Env *)calloc(1, sizeof(Env));
    env->parent = parent;
    return env;
}

/*
 * env_define — 在当前环境层定义新绑定（local 声明用）。
 *
 * 不查找父环境，只在当前 env 的 bindings 链表头部插入。
 * 同名变量可以重复定义（内层 shadow 外层）。
 */
void env_define(Env *env, const char *name, lua_Value value) {
    Binding *b = (Binding *)malloc(sizeof(Binding));
    b->name = strdup(name);
    b->value = value;
    b->next = env->bindings;
    env->bindings = b;
}

/*
 * env_lookup — 查找变量，返回值的指针（允许读写）。
 *
 * 沿 parent 链向上搜索，找到第一个匹配的绑定。
 * 返回指针而非值，这样赋值可以通过 *slot = new_value 修改。
 * 找不到返回 NULL（全局变量未定义）。
 */
lua_Value *env_lookup(Env *env, const char *name) {
    for (Env *e = env; e != NULL; e = e->parent) {
        for (Binding *b = e->bindings; b != NULL; b = b->next) {
            if (strcmp(b->name, name) == 0)
                return &b->value;
        }
    }
    return NULL;
}

/*
 * env_assign — 赋值：查找已有绑定并修改，找不到则在当前 env 定义。
 *
 * 对比 env_define：env_define 总是在当前层新建，env_assign 先查找再决定。
 */
void env_assign(Env *env, const char *name, lua_Value value) {
    lua_Value *slot = env_lookup(env, name);
    if (slot) *slot = value;
    else env_define(env, name, value);
}