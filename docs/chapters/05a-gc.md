# 阶段 5a：标记-清除 GC

> 源码目录：`stage5/lgc.h`、`stage5/lgc.c`
> 官方对照：lua-5.1.5/src/lgc.c
> 测试：08_gc.lua 通过

## 目标

实现垃圾回收，自动释放不再引用的对象（table、function、upvalue、thread）。

## GCObject 公共头

所有可回收对象以 `GCObject` 开头：

```c
struct GCObject {
    int marked;    // 0=白色(未标记), 1=黑色(已标记)
    int gc_type;   // LUA_TTABLE / LUA_TFUNCTION / LUA_TUPVAL / LUA_TTHREAD
    GCObject *next; // 链表指针，串起所有 GC 对象
};
```

`Function`、`Table`、`UpVal`、`lua_Thread` 都以 `GCObject gc` 作为第一个字段。

## Mark-and-Sweep

### Mark

从根集出发，递归标记所有可达对象：

```
根集 = 全局变量 + 所有帧的寄存器 + open upvals + 当前协程
```

`gc_mark_object` 按类型递归：
- **Table**：标记所有 key/value + metatable
- **Function**：标记所有 upval
- **UpVal**：标记 value
- **Thread**：标记所有帧的寄存器 + open upvals

### Sweep

遍历 GC 对象链表，释放未标记对象，重置已标记对象。

```c
void gc_collect(lua_State *L) {
    gc_mark_roots(L);   // 标记
    gc_sweep(L);        // 清除
    L->gc_threshold = L->gc_count * 2;  // 动态调整阈值
}
```

### 触发时机

`OP_NEWTABLE` 时调用 `gc_check`：`gc_count >= gc_threshold` 则触发 GC。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| 算法 | 增量式三色（白/灰/黑） | stop-the-world 两色 |
| 执行 | 分步与 VM 交替 | 一次性 mark+sweep |
| 写屏障 | 有（luaC_barrier） | 无 |
| 弱表 | `__mode` | 无 |
| finalizer | `__gc` | 无 |
| 内存统计 | 按字节 | 按对象计数 |

### 为什么简化？

增量 GC 的核心难点是**写屏障**：在 mark 过程中，黑色对象引用了白色对象，需要把白色对象变灰。我们 stop-the-world 不需要写屏障——mark 时 VM 暂停，不会有新引用产生。

教学上，mark-and-sweep 的概念完整保留，省去的是"不暂停 VM"的工程复杂度。