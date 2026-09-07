/*
 * ltablib.c - table 库（table.insert / table.remove / table.concat / table.sort）
 * 官方对照: lua-5.1.5/src/ltablib.c
 * 实现阶段: 阶段 3+
 *
 * table 库操作 table 的数组部分（整数键 1..n）。
 *
 * table.insert(t, [pos,] v) — 在 pos 位置插入 v（默认末尾），后移元素
 * table.remove(t [, pos])   — 移除并返回 pos 位置元素（默认末尾），前移元素
 * table.concat(t [, sep])   — 拼接所有字符串/数字，用 sep 分隔（默认无分隔）
 * table.sort(t [, comp])    — 原地排序（简化：冒泡排序）
 *
 * 注意：这些函数需要访问全局的 call_function 来调用 comp，
 *       但 builtin 函数签名只接收 args/nargs，不接收 lua_State。
 *       简化：table.sort 暂不支持自定义比较函数（只用 < 比较）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobject.h"
#include "lauxlib.h"
#include "ltable.h"

/*
 * table.insert(t, [pos,] v)
 *
 * 两种形式：
 *   table.insert(t, v)       — 追加到末尾（n+1 位置）
 *   table.insert(t, pos, v)  — 插入到 pos 位置，pos..n 后移到 pos+1..n+1
 */
static lua_Value builtin_insert(lua_Value *args, int nargs) {
    if (nargs < 2 || args[0].type != LUA_TTABLE) {
        fprintf(stderr, "table.insert: 参数错误\n");
        exit(1);
    }
    Table *t = args[0].v.t;
    int n = table_len(t);
    int pos;
    lua_Value value;

    if (nargs >= 3) {
        /* table.insert(t, pos, v) */
        pos = (int)args[1].v.n;
        value = args[2];
    } else {
        /* table.insert(t, v) */
        pos = n + 1;
        value = args[1];
    }

    /* 后移 pos..n → pos+1..n+1 */
    for (int i = n; i >= pos; i--) {
        lua_Value key = make_number(i);
        lua_Value *slot = table_get(t, key);
        lua_Value next_key = make_number(i + 1);
        table_set(t, next_key, slot ? *slot : make_nil());
    }
    table_set(t, make_number(pos), value);
    return make_nil();
}

/*
 * table.remove(t [, pos])
 *
 * 移除 pos 位置元素（默认末尾），前移后续元素，返回被移除的值。
 */
static lua_Value builtin_remove(lua_Value *args, int nargs) {
    if (nargs < 1 || args[0].type != LUA_TTABLE) {
        fprintf(stderr, "table.remove: 参数错误\n");
        exit(1);
    }
    Table *t = args[0].v.t;
    int n = table_len(t);
    if (n == 0) return make_nil();

    int pos = (nargs >= 2) ? (int)args[1].v.n : n;
    lua_Value key = make_number(pos);
    lua_Value *slot = table_get(t, key);
    lua_Value removed = slot ? *slot : make_nil();

    /* 前移 pos+1..n → pos..n-1 */
    for (int i = pos; i < n; i++) {
        lua_Value src_key = make_number(i + 1);
        lua_Value dst_key = make_number(i);
        lua_Value *src = table_get(t, src_key);
        table_set(t, dst_key, src ? *src : make_nil());
    }
    /* 清除原末尾 */
    table_set(t, make_number(n), make_nil());
    return removed;
}

/*
 * table.concat(t [, sep])
 *
 * 拼接 t[1]..t[n]，用 sep 分隔（默认无分隔）。
 * 元素必须是字符串或数字。
 */
static lua_Value builtin_concat(lua_Value *args, int nargs) {
    if (nargs < 1 || args[0].type != LUA_TTABLE) {
        fprintf(stderr, "table.concat: 参数错误\n");
        exit(1);
    }
    Table *t = args[0].v.t;
    const char *sep = NULL;
    if (nargs >= 2 && args[1].type == LUA_TSTRING)
        sep = args[1].v.s;

    int n = table_len(t);
    /* 先计算总长度 */
    size_t total = 1;  /* '\0' */
    for (int i = 1; i <= n; i++) {
        lua_Value *slot = table_get(t, make_number(i));
        if (!slot) continue;
        if (slot->type == LUA_TSTRING)
            total += strlen(slot->v.s);
        else if (slot->type == LUA_TNUMBER) {
            char buf[64];
            if (slot->v.n == (double)(long)slot->v.n)
                snprintf(buf, sizeof(buf), "%ld", (long)slot->v.n);
            else
                snprintf(buf, sizeof(buf), "%g", slot->v.n);
            total += strlen(buf);
        }
        if (sep && i < n) total += strlen(sep);
    }

    /* 拼接 */
    char *result = (char *)malloc(total);
    result[0] = '\0';
    for (int i = 1; i <= n; i++) {
        lua_Value *slot = table_get(t, make_number(i));
        if (!slot) continue;
        if (slot->type == LUA_TSTRING)
            strcat(result, slot->v.s);
        else if (slot->type == LUA_TNUMBER) {
            char buf[64];
            if (slot->v.n == (double)(long)slot->v.n)
                snprintf(buf, sizeof(buf), "%ld", (long)slot->v.n);
            else
                snprintf(buf, sizeof(buf), "%g", slot->v.n);
            strcat(result, buf);
        }
        if (sep && i < n) strcat(result, sep);
    }
    return make_string_owned(result);
}

/*
 * table.sort(t)
 *
 * 原地排序（冒泡排序，简化版，不支持自定义比较函数）。
 * 用 < 运算比较：数字按大小，字符串按字典序。
 */
static lua_Value builtin_sort(lua_Value *args, int nargs) {
    if (nargs < 1 || args[0].type != LUA_TTABLE) {
        fprintf(stderr, "table.sort: 参数错误\n");
        exit(1);
    }
    Table *t = args[0].v.t;
    int n = table_len(t);

    /* 冒泡排序 */
    for (int i = 1; i < n; i++) {
        for (int j = 1; j <= n - i; j++) {
            lua_Value kj = make_number(j);
            lua_Value kj1 = make_number(j + 1);
            lua_Value *a = table_get(t, kj);
            lua_Value *b = table_get(t, kj1);
            if (!a || !b) continue;

            /* 比较 a < b？ */
            int swap = 0;
            if (a->type == LUA_TNUMBER && b->type == LUA_TNUMBER)
                swap = a->v.n > b->v.n;
            else if (a->type == LUA_TSTRING && b->type == LUA_TSTRING)
                swap = strcmp(a->v.s, b->v.s) > 0;

            if (swap) {
                /* 注意：a/b 是指向哈希表内部的指针，table_set 后可能失效。
                 * 先保存值再写入。 */
                lua_Value tmp = *a;
                table_set(t, kj, *b);
                table_set(t, kj1, tmp);
            }
        }
    }
    return make_nil();
}

/*
 * open_tablib — 注册 table 库。
 *
 * 创建一个全局 table，把所有 table.* 函数放进去。
 */
void open_tablib(Env *env) {
    /* 创建 table 库的 table */
    Table *lib = table_create();
    table_set(lib, make_string("insert"), make_function(make_builtin(builtin_insert, "insert")));
    table_set(lib, make_string("remove"), make_function(make_builtin(builtin_remove, "remove")));
    table_set(lib, make_string("concat"), make_function(make_builtin(builtin_concat, "concat")));
    table_set(lib, make_string("sort"),   make_function(make_builtin(builtin_sort,   "sort")));
    env_define(env, "table", make_table(lib));
}
