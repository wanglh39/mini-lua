/*
 * ltable.c - table 实现（数组+哈希合一、rehash 分区策略 luaH_resize）
 * 官方对照: lua-5.1.5/src/ltable.c
 * 实现阶段: 阶段 3+
 *
 * 纯哈希实现（开放寻址法，线性探测）。
 *
 * 哈希函数：
 *   数字 — 位运算散列
 *   字符串 — djb2 算法
 *   其他 — 指针地址
 *
 * 扩容：当 count > size * 3/4 时，扩到 2*size 并 rehash。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ltable.h"

/* ─── 哈希函数 ─── */

static unsigned int hash_value(lua_Value v) {
    switch (v.type) {
        case LUA_TNUMBER: {
            double n = v.v.n;
            if (n == (double)(long)n)
                return (unsigned int)(long)n * 2654435761u;
            /* 用 memcpy 避免严格别名违规（-O2 下 UB） */
            unsigned long long bits;
            memcpy(&bits, &n, sizeof(bits));
            return (unsigned int)bits;
        }
        case LUA_TSTRING: {
            /* djb2 字符串哈希 */
            unsigned int h = 5381;
            const char *s = v.v.s;
            while (*s) h = h * 33 + (unsigned char)*s++;
            return h;
        }
        case LUA_TBOOLEAN:
            return (unsigned int)v.v.b;
        case LUA_TFUNCTION:
            return (unsigned int)(uintptr_t)v.v.fn;
        case LUA_TTABLE:
            return (unsigned int)(uintptr_t)v.v.t;
        default:
            return 0;
    }
}

/* 值的相等判断（table 专用，和 value_equals 类似但内联更快） */
static int key_equals(lua_Value a, lua_Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case LUA_TNIL:      return 1;
        case LUA_TBOOLEAN:  return a.v.b == b.v.b;
        case LUA_TNUMBER:   return a.v.n == b.v.n;
        case LUA_TSTRING:   return strcmp(a.v.s, b.v.s) == 0;
        case LUA_TFUNCTION: return a.v.fn == b.v.fn;
        case LUA_TTABLE:    return a.v.t == b.v.t;
        default:            return 0;
    }
}

/* ─── Table 操作 ─── */

Table *table_create(void) {
    Table *t = (Table *)calloc(1, sizeof(Table));
    t->size = 8;  /* 初始大小 */
    t->count = 0;
    t->nodes = (Node *)calloc(t->size, sizeof(Node));
    t->metatable = NULL;
    return t;
}

/*
 * 查找键的位置。
 *
 * 返回指向 Node 的指针。如果 found 不为 NULL，*found 设为是否找到。
 * 用于 get（找到返回 value 指针）和 set（找到更新，没找到插入到空槽）。
 */
static Node *find_node(Table *t, lua_Value key, int *found) {
    unsigned int h = hash_value(key) & (t->size - 1);
    for (int i = 0; i < t->size; i++) {
        Node *n = &t->nodes[(h + i) & (t->size - 1)];
        if (!n->used) {
            if (found) *found = 0;
            return n;  /* 返回第一个空槽 */
        }
        if (key_equals(n->key, key)) {
            if (found) *found = 1;
            return n;
        }
    }
    if (found) *found = 0;
    return NULL;  /* 表满（不应发生，因为会提前扩容） */
}

lua_Value *table_get(Table *t, lua_Value key) {
    if (key.type == LUA_TNIL) return NULL;  /* nil 键无意义 */
    int found;
    Node *n = find_node(t, key, &found);
    if (found && n)
        return &n->value;
    return NULL;
}

/* 扩容并 rehash */
static void table_resize(Table *t, int new_size) {
    Node *old_nodes = t->nodes;
    int old_size = t->size;
    t->nodes = (Node *)calloc(new_size, sizeof(Node));
    t->size = new_size;
    t->count = 0;
    for (int i = 0; i < old_size; i++) {
        if (old_nodes[i].used) {
            int found;
            Node *n = find_node(t, old_nodes[i].key, &found);
            n->key = old_nodes[i].key;
            n->value = old_nodes[i].value;
            n->used = 1;
            t->count++;
        }
    }
    free(old_nodes);
}

void table_set(Table *t, lua_Value key, lua_Value value) {
    if (key.type == LUA_TNIL) return;  /* nil 键无意义 */

    /* 扩容检查 */
    if (t->count >= t->size * 3 / 4)
        table_resize(t, t->size * 2);

    int found;
    Node *n = find_node(t, key, &found);
    if (!found) {
        n->key = key;
        n->used = 1;
        t->count++;
    }
    n->value = value;
}

/*
 * table_len — #t 运算。
 *
 * 找最大的 n 使得 t[1]..t[n] 都非 nil。
 * 简化：线性扫描 1, 2, 3, ... 直到遇到 nil 或不存在的键。
 * 官方 Lua 用二分搜索，更快但复杂。
 */
int table_len(Table *t) {
    int n = 0;
    lua_Value key = make_number(1);
    lua_Value *slot;
    while ((slot = table_get(t, key)) != NULL && slot->type != LUA_TNIL) {
        n++;
        key.v.n = n + 1;
    }
    return n;
}

/*
 * table_next — pairs/next 遍历。
 *
 * 给定 key（nil 表示从头），返回下一个非空槽的 key 指针。
 * 遍历顺序不保证（哈希表特性）。
 */
lua_Value *table_next(Table *t, lua_Value key) {
    int start = 0;
    if (key.type != LUA_TNIL) {
        /* 找到 key 的位置，从下一个开始 */
        int found;
        Node *n = find_node(t, key, &found);
        if (!found || !n) return NULL;
        start = (int)(n - t->nodes) + 1;
    }
    for (int i = start; i < t->size; i++) {
        if (t->nodes[i].used)
            return &t->nodes[i].key;
    }
    return NULL;  /* 遍历结束 */
}
