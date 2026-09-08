/*
 * ltm.h - 元方法表（TMS 枚举、luaT_gettm 查元方法）
 * 官方对照: lua-5.1.5/src/ltm.h
 * 实现阶段: 阶段 3+
 *
 * Metatable 是 Lua 的 OOP 基石：
 *   每个 table 可以有一个 metatable（另一个 table），
 *   metatable 中的特殊键（__index, __newindex, __add 等）定义了该 table 的"行为"。
 *
 * __index：当 t[k] 不存在时，查 metatable.__index：
 *   - 若是 table，递归查 __index[k]
 *   - 若是 function，调用 __index(t, k)
 *
 * __newindex：当 t[k] 不存在时（k 不是已有键），查 metatable.__newindex：
 *   - 若是 table，在 __newindex 上设置 k=v
 *   - 若是 function，调用 __newindex(t, k, v)
 *
 * 这实现了继承、代理、只读 table 等模式。
 */

#ifndef ltm_h
#define ltm_h

#include "lobject.h"

/*
 * 查找 table 的元方法。
 *
 * 在 t 的 metatable 中查找 event 名对应的值。
 * 返回指向值的指针，或 NULL（无 metatable 或元方法不存在）。
 */
lua_Value *get_metamethod(Table *t, const char *event);

#endif /* ltm_h */
