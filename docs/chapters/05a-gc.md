# 阶段 5a：标记-清除 GC

> 源码目录：`stage5/lgc.h`、`stage5/lgc.c`
> 官方对照：lua-5.1.5/src/lgc.c
> 测试：08_gc.lua 通过

## 目标

实现垃圾回收，自动释放不再引用的对象（table、function、upvalue、thread）。

前四个阶段都是 malloc 不 free——内存只增不减。阶段 5 加入 GC，让长跑程序不会内存爆炸。

## GCObject 公共头

所有可回收对象以 `GCObject` 开头：

```c
struct GCObject {
    int marked;     // 0=白色(未标记), 1=黑色(已标记)
    int gc_type;    // LUA_TTABLE / LUA_TFUNCTION / LUA_TUPVAL / LUA_TTHREAD
    GCObject *next; // 链表指针，串起所有 GC 对象
};
```

`Function`、`Table`、`UpVal`、`lua_Thread` 都以 `GCObject gc` 作为**第一个字段**。这样可以用 `(GCObject*)obj` 统一遍历所有类型——C 的 struct 首字段地址等于 struct 地址。

```c
struct Function { GCObject gc; int is_builtin; ... };
struct Table    { GCObject gc; Node *nodes; ... };
struct UpVal    { GCObject gc; lua_Value *ptr; ... };
struct lua_Thread { GCObject gc; lua_State *L; ... };
```

### gc_register

新对象创建时注册到 GC 链表：

```c
void gc_register(lua_State *L, GCObject *obj, int gc_type) {
    obj->marked = 0;      // 白色
    obj->gc_type = gc_type;
    obj->next = L->gc_head;
    L->gc_head = obj;     // 头插法
    L->gc_count++;
}
```

## Mark-and-Sweep 算法

### Mark：从根集标记可达对象

```
根集 = 全局变量 + 所有帧的寄存器 + open upvals + 当前协程
```

```c
static void gc_mark_roots(lua_State *L) {
    // 1. 全局环境
    for (Env *e = L->globals; e; e = e->parent)
        for (Binding *b = e->bindings; b; b = b->next)
            gc_mark_value(L, b->value);

    // 2. 所有调用帧的寄存器
    for (int i = 0; i < L->frame_count; i++) {
        CallFrame *cf = &L->frames[i];
        int nregs = cf->fn->u.user.proto->nregs;
        for (int r = 0; r < nregs; r++)
            gc_mark_value(L, cf->regs[r]);
    }

    // 3. open upvalue 链表
    for (UpVal *uv = L->open_upvals; uv; uv = uv->next)
        gc_mark_object(L, &uv->gc);

    // 4. 当前协程
    if (g_current_thread)
        gc_mark_object(L, &g_current_thread->gc);
}
```

### gc_mark_value — 标记一个值

```c
void gc_mark_value(lua_State *L, lua_Value v) {
    switch (v.type) {
        case LUA_TTABLE:    gc_mark_object(L, &v.v.t->gc);  break;
        case LUA_TFUNCTION: gc_mark_object(L, &v.v.fn->gc); break;
        case LUA_TTHREAD:   gc_mark_object(L, &v.v.th->gc); break;
        default: break;  // nil/bool/number/string 不需要标记
    }
}
```

### gc_mark_object — 递归标记对象及其子对象

```c
void gc_mark_object(lua_State *L, GCObject *obj) {
    if (obj->marked) return;  // 已标记，跳过
    obj->marked = 1;          // 标记为黑色

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
            if (!fn->is_builtin && fn->u.user.nupvals > 0)
                for (int i = 0; i < fn->u.user.nupvals; i++)
                    gc_mark_object(L, &fn->u.user.upvals[i]->gc);
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
                // 标记协程所有帧的寄存器
                for (int i = 0; i < th->L->frame_count; i++) {
                    CallFrame *cf = &th->L->frames[i];
                    int nregs = cf->fn->u.user.proto->nregs;
                    for (int r = 0; r < nregs; r++)
                        gc_mark_value(L, cf->regs[r]);
                }
                // 标记 open upvals
                for (UpVal *uv = th->L->open_upvals; uv; uv = uv->next)
                    gc_mark_object(L, &uv->gc);
            }
            if (th->fn)
                gc_mark_object(L, &th->fn->gc);
            break;
        }
    }
}
```

### Sweep：清除未标记对象

```c
static void gc_sweep(lua_State *L) {
    GCObject **pp = &L->gc_head;
    while (*pp) {
        GCObject *obj = *pp;
        if (obj->marked) {
            obj->marked = 0;     // 重置为白色，为下次 GC 准备
            pp = &obj->next;
        } else {
            *pp = obj->next;     // 从链表移除
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
            }
        }
    }
}
```

### 完整 GC 周期

```c
void gc_collect(lua_State *L) {
    gc_mark_roots(L);   // 标记
    gc_sweep(L);        // 清除
    L->gc_threshold = L->gc_count * 2;  // 动态调整阈值
    if (L->gc_threshold < 100) L->gc_threshold = 100;
}
```

### 触发时机

`OP_NEWTABLE` 时调用 `gc_check`：

```c
void gc_check(lua_State *L) {
    if (L->gc_count >= L->gc_threshold)
        gc_collect(L);
}
```

阈值动态调整：GC 后 `threshold = count * 2`，让下次 GC 在对象数翻倍时才触发。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| 算法 | **增量式三色**（白/灰/黑） | **stop-the-world 两色**（白/黑） |
| 执行 | 分步与 VM 交替，不暂停 | 一次性 mark+sweep |
| 写屏障 | 有（luaC_barrier） | 无 |
| 弱表 | `__mode = "k"/"v"` | 无 |
| finalizer | `__gc` 元方法 | 无 |
| 内存统计 | 按字节（luaM_realloc 维护） | 按对象计数 |

### 为什么简化？

增量 GC 的核心难点是**写屏障**：在 mark 过程中，黑色对象引用了白色对象，需要把白色对象变灰。我们 stop-the-world 不需要写屏障——mark 时 VM 暂停，不会有新引用产生。

三色（白/灰/黑）的灰色用于增量标记：灰色对象已发现但子对象未遍历。我们一次性遍历完，不需要灰色。

教学上，mark-and-sweep 的概念完整保留，省去的是"不暂停 VM"的工程复杂度。

## 测试

```lua
-- 08_gc.lua
local function create_garbage()
    for i = 1, 10000 do
        local t = {x = i, y = i + 1, z = i + 2}  -- 创建大量临时 table
    end  -- t 离开作用域，可回收
end
create_garbage()
print("GC test 1 passed")  -- 无内存爆炸

-- 保留的对象不被回收
local keep = {}
for i = 1, 100 do
    keep[i] = {value = i}  -- 这些必须保留
end
for i = 1, 10000 do
    local junk = {x = i}  -- 这些可以回收
end
print(keep[1].value)   -- 1
print(keep[50].value)  -- 50
print(keep[100].value) -- 100
```

GC 正确回收临时对象，保留可达对象。
