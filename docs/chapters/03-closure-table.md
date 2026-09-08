# 阶段 3：闭包 + table + metatable

> 源码目录：`stage3/`
> 官方对照：lua-5.1.5/src/ltable.c、ltm.c、lfunc.c
> 测试：01-06 全部通过

## 目标

加入 table（Lua 唯一的数据结构）、metatable（OOP 基础）、正确的闭包语义（upvalue）。

## Table：纯哈希

官方 Lua 的 Table 是 array + hash 混合：整数键 1..n 存数组部分（缓存友好），其余存 hash。

我们简化为**纯哈希**（开放寻址法 + 线性探测）：

```c
struct Table {
    Node *nodes;   // 哈希节点数组
    int size;      // 2 的幂
    int count;     // 已占用条目数
    Table *metatable;
};
```

扩容条件：`count > size * 3/4` → 扩到 `2*size` 并 rehash。

## Upvalue：open/closed 状态机

这是 Lua 闭包实现的精髓。

```c
struct UpVal {
    lua_Value *ptr;   // open: 指向栈槽; closed: 指向 &value
    lua_Value value;  // closed 时的值
    UpVal *next;      // open upvalue 链表
};
```

- **open 状态**：外层函数还没返回，`ptr` 指向外层帧的寄存器槽位
- **closed 状态**：外层函数已返回，值复制到 `value`，`ptr` 改为 `&value`

函数返回时 `close_upvals` 把所有指向该帧的 open UpVal 关闭。

### 为什么用引用而非值拷贝？

同一外层变量被多个闭包捕获时共享同一个 UpVal。一个闭包修改它，其他闭包也能看到。

## Metatable

`table_getindex` 查找顺序：
1. 直接在 t 中查 key
2. 若不存在，查 `t.metatable.__index`：
   - 是 table → 递归查
   - 是 function → 调用 `__index(t, key)`
3. 仍不存在 → nil

`table_setindex` 类似，查 `__newindex`。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| Table | array + hash 混合 | 纯 hash |
| UpVal | 一致 | 一致 |
| Metatable | 一致 | 一致 |
| 哈希函数 | 针对每种类型优化 | djb2（字符串）+ 位运算（数字） |