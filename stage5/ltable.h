/*
 * ltable.h - table 操作接口（luaH_get / luaH_set / luaH_resize）
 * 官方对照: lua-5.1.5/src/ltable.h
 * 实现阶段: 阶段 3+
 *
 * Table 是 Lua 唯一的数据结构——数组、字典、对象、命名空间都用它。
 *
 * 官方 Lua 的 Table 是数组+哈希合一：
 *   整数键 1..n 存在数组部分（连续内存，缓存友好），
 *   其余键存在哈希部分。
 *   rehash 时重新划分哪些键进数组、哪些进哈希。
 *
 * 阶段 3 简化：纯哈希实现（开放寻址法，线性探测）。
 *   所有键统一用哈希处理，不做数组/哈希分离。
 *   教学上更简单，性能足够。官方的分离策略在 docs/vs-official/ 讨论。
 */

#ifndef ltable_h
#define ltable_h

#include "lobject.h"

/*
 * 哈希节点。
 *
 * used=0 表示空槽，used=1 表示占用。
 * 开放寻址法：冲突时线性探测下一个槽。
 */
typedef struct Node {
    lua_Value key;
    lua_Value value;
    int used;
} Node;

/*
 * Table 结构。
 *
 * nodes     — 哈希节点数组
 * size      — 哈希表大小（2 的幂，用于位运算取模）
 * count     — 已占用条目数
 * metatable — 元表（另一个 Table，可为 NULL）
 */
struct Table {
    GCObject gc;     /* GC 头（必须第一个） */
    Node *nodes;
    int size;
    int count;
    struct Table *metatable;
};

/* 创建空 table */
Table *table_create(void);

/* 查找：返回指向 value 的指针，或 NULL（键不存在） */
lua_Value *table_get(Table *t, lua_Value key);

/* 设置键值对（键已存在则更新，不存在则插入） */
void table_set(Table *t, lua_Value key, lua_Value value);

/*
 * 取长度：#t
 * 返回最大的 n，使得 t[1], t[2], ..., t[n] 都不为 nil。
 * 这是 Lua 的 # 运算语义（用于数组长度）。
 */
int table_len(Table *t);

/*
 * next：用于 pairs() 遍历。
 * 给定 key（nil 表示从头开始），返回下一个 key 的指针，或 NULL（遍历结束）。
 */
lua_Value *table_next(Table *t, lua_Value key);

#endif /* ltable_h */
