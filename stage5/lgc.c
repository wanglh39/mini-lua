/*
 * lgc.c - 垃圾回收器实现（mark-and-sweep）
 * 官方对照: lua-5.1.5/src/lgc.c
 * 实现阶段: 阶段 5
 *
 * 实现：
 *   gc_register — 新对象加入 GC 链表
 *   gc_mark_value — 标记一个 lua_Value 引用的 GC 对象
 *   gc_mark_object — 递归标记 GC 对象及其子对象
 *   gc_collect — mark + sweep 完整周期
 *   gc_check — 分配计数超阈值时触发 GC
 *
 * 根集：
 *   1. 全局环境（globals 的所有 binding）
 *   2. 所有调用帧的寄存器（regs[0..nregs-1]）
 *   3. open upvalue 链表
 *   4. 当前协程
 */

#include <stdio.h>
#include <stdlib.h>
#include "lgc.h"
#include "ltable.h"

/* ─── 注册 ─── */

void gc_register(lua_State *L, GCObject *obj, int gc_type) {
    obj->marked = 0;  /* 白色 */
    obj->gc_type = gc_type;
    obj->next = L->gc_head;
    L->gc_head = obj;
    L->gc_count++;
}

/* ─── 标记 ─── */

void gc_mark_value(lua_State *L, lua_Value v) {
    switch (v.type) {
        case LUA_TTABLE:
            gc_mark_object(L, &v.v.t->gc);
            break;
        case LUA_TFUNCTION:
            gc_mark_object(L, &v.v.fn->gc);
            break;
        case LUA_TTHREAD:
            gc_mark_object(L, &v.v.th->gc);
            break;
        default:
            break;
    }
}

void gc_mark_object(lua_State *L, GCObject *obj) {
    if (obj->marked) return;
    obj->marked = 1;

    switch (obj->gc_type) {
        case LUA_TTABLE: {
            Table *t = (Table *)obj;
            for (int i = 0; i < t->size; i++) {
                if (t->nodes[i].used) {
                    gc_mark_value(L, t->nodes[i].key);
                    gc_mark_value(L, t->nodes[i].value);
                }
            }
            if (t->metatable)
                gc_mark_object(L, &t->metatable->gc);
            break;
        }
        case LUA_TFUNCTION: {
            Function *fn = (Function *)obj;
            if (!fn->is_builtin && fn->u.user.nupvals > 0) {
                for (int i = 0; i < fn->u.user.nupvals; i++)
                    gc_mark_object(L, &fn->u.user.upvals[i]->gc);
            }
            break;
        }
        case LUA_TUPVAL: {
            UpVal *uv = (UpVal *)obj;
            gc_mark_value(L, uv->value);
            break;
        }
        case LUA_TTHREAD: {
            lua_Thread *th = (lua_Thread *)obj;
            if (th->L) {
                for (int i = 0; i < th->L->frame_count; i++) {
                    CallFrame *cf = &th->L->frames[i];
                    if (cf->fn && !cf->fn->is_builtin) {
                        int nregs = cf->fn->u.user.proto->nregs;
                        for (int r = 0; r < nregs; r++)
                            gc_mark_value(L, cf->regs[r]);
                    }
                }
                for (UpVal *uv = th->L->open_upvals; uv; uv = uv->next)
                    gc_mark_object(L, &uv->gc);
            }
            if (th->fn)
                gc_mark_object(L, &th->fn->gc);
            break;
        }
    }
}

/* ─── 标记根集 ─── */

static void gc_mark_roots(lua_State *L) {
    for (Env *e = L->globals; e; e = e->parent) {
        for (Binding *b = e->bindings; b; b = b->next)
            gc_mark_value(L, b->value);
    }
    for (int i = 0; i < L->frame_count; i++) {
        CallFrame *cf = &L->frames[i];
        if (cf->fn && !cf->fn->is_builtin) {
            int nregs = cf->fn->u.user.proto->nregs;
            for (int r = 0; r < nregs; r++)
                gc_mark_value(L, cf->regs[r]);
        }
    }
    for (UpVal *uv = L->open_upvals; uv; uv = uv->next)
        gc_mark_object(L, &uv->gc);
    if (g_current_thread)
        gc_mark_object(L, &g_current_thread->gc);
}

/* ─── 清除 ─── */

static void gc_sweep(lua_State *L) {
    GCObject **pp = &L->gc_head;
    while (*pp) {
        GCObject *obj = *pp;
        if (obj->marked) {
            obj->marked = 0;
            pp = &obj->next;
        } else {
            *pp = obj->next;
            L->gc_count--;
            switch (obj->gc_type) {
                case LUA_TTABLE: {
                    Table *t = (Table *)obj;
                    free(t->nodes);
                    free(t);
                    break;
                }
                case LUA_TFUNCTION: {
                    Function *fn = (Function *)obj;
                    if (!fn->is_builtin && fn->u.user.nupvals > 0)
                        free(fn->u.user.upvals);
                    free(fn);
                    break;
                }
                case LUA_TUPVAL:
                    free(obj);
                    break;
                case LUA_TTHREAD: {
                    lua_Thread *th = (lua_Thread *)obj;
                    if (th->L) state_free(th->L);
                    free(th);
                    break;
                }
                default:
                    free(obj);
                    break;
            }
        }
    }
}

/* ─── 完整 GC 周期 ─── */

void gc_collect(lua_State *L) {
    gc_mark_roots(L);
    gc_sweep(L);
    L->gc_threshold = L->gc_count * 2;
    if (L->gc_threshold < 100) L->gc_threshold = 100;
}

void gc_check(lua_State *L) {
    if (L->gc_count >= L->gc_threshold)
        gc_collect(L);
}