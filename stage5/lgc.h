/*
 * lgc.h - 垃圾回收器接口（mark-and-sweep）
 * 官方对照: lua-5.1.5/src/lgc.h
 * 实现阶段: 阶段 5
 *
 * 简化 mark-and-sweep GC（非增量，stop-the-world）：
 *   1. Mark: 从根集（全局变量/帧寄存器/open upvals）出发，标记所有可达对象
 *   2. Sweep: 遍历 GC 对象链表，释放未标记对象，重置已标记对象
 *
 * 对比官方 Lua 的增量 GC：
 *   官方用三色标记（白/灰/黑）+ 写屏障，分步执行不暂停 VM。
 *   阶段 5 简化为 stop-the-world：GC 时暂停执行，一次性完成 mark+sweep。
 *   教学上更简单，概念相同。
 */

#ifndef lgc_h
#define lgc_h

#include "lobject.h"
#include "lvm.h"

/* 注册 GC 对象到链表 */
void gc_register(lua_State *L, GCObject *obj, int gc_type);

/* 标记一个值（如果是 GC 对象则标记） */
void gc_mark_value(lua_State *L, lua_Value v);

/* 标记一个 GC 对象及其所有子对象 */
void gc_mark_object(lua_State *L, GCObject *obj);

/* 完整 GC 周期：mark + sweep */
void gc_collect(lua_State *L);

/* 检查是否需要 GC，需要则执行 */
void gc_check(lua_State *L);

#endif /* lgc_h */