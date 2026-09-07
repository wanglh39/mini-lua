/*
 * lgc.c - GC 实现（三色标记、增量步进 luaC_step、写屏障 luaC_barrier、字符串特殊处理）
 * 官方对照: lua-5.1.5/src/lgc.c
 * 实现阶段: 阶段 5a
 */

