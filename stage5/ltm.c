/*
 * ltm.c - 元方法表实现（元方法名初始化、快速查找）
 * 官方对照: lua-5.1.5/src/ltm.c
 * 实现阶段: 阶段 3+
 *
 * 简化实现：直接在 metatable 中用字符串键查找元方法。
 * 官方 Lua 把元方法名 intern 成短字符串，用快速指针比较；
 * 我们用 strcmp，慢但简单。
 */

#include <string.h>
#include "ltm.h"
#include "ltable.h"

/*
 * get_metamethod — 查找 table 的元方法。
 *
 * t 有 metatable 且 metatable 中有 event 键时，返回指向值的指针。
 * 否则返回 NULL。
 */
lua_Value *get_metamethod(Table *t, const char *event) {
    if (t == NULL || t->metatable == NULL)
        return NULL;
    lua_Value key = make_string(event);
    return table_get(t->metatable, key);
}
