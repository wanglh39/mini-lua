# 阶段 3：闭包 + table + metatable

> 源码目录：`stage3/`
> 官方对照：lua-5.1.5/src/ltable.c、ltm.c、lfunc.c
> 测试：01-06 全部通过

## 目标

加入 table（Lua 唯一的数据结构）、metatable（OOP 基础）、正确的闭包语义（upvalue）。

这是功能最丰富的一个阶段。Lua 的设计哲学"table 是唯一的数据结构"在这里体现——数组、字典、对象、命名空间全是 table。

## Table：纯哈希实现

### 官方 vs 我们

官方 Lua 的 Table 是 **array + hash 混合**：
- 整数键 1..n 存在数组部分（连续内存，缓存友好）
- 其余键存在哈希部分
- rehash 时重新划分哪些键进数组、哪些进哈希

我们简化为**纯哈希**（开放寻址法 + 线性探测）：

```c
typedef struct Node {
    lua_Value key;
    lua_Value value;
    int used;      // 0=空槽, 1=占用
} Node;

struct Table {
    Node *nodes;   // 哈希节点数组
    int size;      // 2 的幂（位运算取模）
    int count;     // 已占用条目数
    Table *metatable;
};
```

### 哈希函数

```c
static unsigned int hash_value(lua_Value v) {
    switch (v.type) {
        case LUA_TNUMBER: {
            double n = v.v.n;
            if (n == (double)(long)n)
                return (unsigned int)(long)n * 2654435761u;  // 整数：乘法散列
            unsigned long long bits;
            memcpy(&bits, &n, sizeof(bits));  // 浮点：位模式
            return (unsigned int)bits;
        }
        case LUA_TSTRING: {
            unsigned int h = 5381;  // djb2 算法
            const char *s = v.v.s;
            while (*s) h = h * 33 + (unsigned char)*s++;
            return h;
        }
        default:
            return (unsigned int)(uintptr_t)v.v.fn;  // 指针地址
    }
}
```

### 扩容

当 `count > size * 3/4` 时扩容到 `2*size` 并 rehash：

```c
static void table_rehash(Table *t) {
    Node *old_nodes = t->nodes;
    int old_size = t->size;
    t->size *= 2;
    t->nodes = calloc(t->size, sizeof(Node));
    t->count = 0;
    for (int i = 0; i < old_size; i++) {
        if (old_nodes[i].used)
            table_set(t, old_nodes[i].key, old_nodes[i].value);
    }
    free(old_nodes);
}
```

### 为什么用开放寻址而非链地址？

开放寻址法（线性探测）对缓存友好——数据连续存储，CPU 预取效果好。缺点是删除复杂（需要 tombstone），但 Lua 的 table 删除不频繁。

### table_len（# 运算符）

Lua 的 `#t` 返回最大的 n，使得 `t[1], t[2], ..., t[n]` 都不为 nil：

```c
int table_len(Table *t) {
    int n = 0;
    while (1) {
        lua_Value *slot = table_get(t, make_number(n + 1));
        if (!slot || slot->type == LUA_TNIL) break;
        n++;
    }
    return n;
}
```

这是 O(n) 的线性扫描。官方 Lua 用二分搜索做 O(log n)——因为 array 部分是连续的，可以二分。纯哈希做不到。

## Upvalue：open/closed 状态机

这是 Lua 闭包实现的精髓，也是本阶段最复杂的部分。

### 问题

```lua
function counter()
    local count = 0
    return function() count = count + 1; return count end
end

local c = counter()  -- counter 已返回，count 的栈帧没了
c()  -- 1：但 count 从哪来？
```

`counter` 返回后，`count` 的栈帧被销毁。但闭包仍然能访问和修改它。怎么做到的？

### 解决方案：UpVal 引用

```c
struct UpVal {
    lua_Value *ptr;    // open: 指向栈槽; closed: 指向 &value
    lua_Value value;   // closed 时的值
    UpVal *next;       // open upvalue 链表
};
```

两种状态：
- **open**：外层函数还在执行，`ptr` 指向外层帧的寄存器槽位。读写通过 `*ptr` 间接访问。
- **closed**：外层函数已返回，值已复制到 `value`，`ptr` 改为 `&value`。读写仍然通过 `*ptr`，但现在指向堆上的 `value`。

### 状态转换

```
外层函数执行中：open  (ptr → 栈槽)
外层函数返回时：close (复制 *ptr 到 value，ptr = &value)
之后：closed (ptr → &value，堆上)
```

### close_upvals

函数返回时，关闭所有指向该帧的 open UpVal：

```c
static void close_upvals(lua_State *L, lua_Value *base, int n) {
    UpVal **pp = &L->open_upvals;
    while (*pp) {
        UpVal *uv = *pp;
        if (uv->ptr >= base && uv->ptr < base + n) {
            uv->value = *uv->ptr;  // 复制值
            uv->ptr = &uv->value;  // 指向自身
            *pp = uv->next;        // 从 open 链表移除
        } else {
            pp = &uv->next;
        }
    }
}
```

### find_or_create_open_upval

创建闭包时，需要为每个 upvalue 找到或创建 UpVal：

```c
static UpVal *find_or_create_open_upval(lua_State *L, lua_Value *slot) {
    // 查找已有的 open UpVal 指向同一槽位
    for (UpVal *uv = L->open_upvals; uv; uv = uv->next) {
        if (uv->ptr == slot) return uv;  // 复用
    }
    // 创建新的 open UpVal
    UpVal *uv = malloc(sizeof(UpVal));
    uv->ptr = slot;
    uv->next = L->open_upvals;
    L->open_upvals = uv;
    return uv;
}
```

### 为什么用引用而非值拷贝？

同一外层变量被多个闭包捕获时共享同一个 UpVal。一个闭包修改它，其他闭包也能看到：

```lua
function make_pair()
    local x = 0
    local function get() return x end
    local function set(v) x = v end
    return get, set
end

local get, set = make_pair()
set(42)
print(get())  -- 42：get 和 set 共享同一个 UpVal
```

## Metatable

metatable 是 table 的"类型元信息"，实现 OOP 和运算符重载。

### __index 元方法

`table_getindex` 查找顺序：

```c
static lua_Value table_getindex(lua_State *L, lua_Value tbl, lua_Value key) {
    // 1. 直接在 t 中查 key
    lua_Value *slot = table_get(tbl.v.t, key);
    if (slot) return *slot;

    // 2. 查 metatable.__index
    lua_Value *mm = get_metamethod(tbl.v.t, "__index");
    if (mm && mm->type == LUA_TTABLE)
        return table_getindex(L, *mm, key);  // 递归查
    if (mm && mm->type == LUA_TFUNCTION) {
        lua_Value args[2] = {tbl, key};
        return call_function(L, mm->v.fn, args, 2);  // 调用 __index(t, key)
    }
    // 3. 不存在 → nil
    return make_nil();
}
```

### __newindex 元方法

`table_setindex` 类似，在 key 不存在时查 `__newindex`：

```c
static void table_setindex(lua_State *L, lua_Value tbl, lua_Value key, lua_Value value) {
    if (table_get(tbl.v.t, key) != NULL) {
        table_set(tbl.v.t, key, value);  // 键已存在，直接更新
    } else {
        lua_Value *mm = get_metamethod(tbl.v.t, "__newindex");
        if (mm && mm->type == LUA_TTABLE)
            table_set(mm->v.t, key, value);  // 委托给 __newindex table
        else if (mm && mm->type == LUA_TFUNCTION) {
            lua_Value args[3] = {tbl, key, value};
            call_function(L, mm->v.fn, args, 3);  // 调用 __newindex(t, key, value)
        } else {
            table_set(tbl.v.t, key, value);  // 无元方法，直接设
        }
    }
}
```

### OOP 示例

```lua
Animal = {}
Animal.__index = Animal  -- 让 Animal 成为元表

function Animal.new(name)
    local self = setmetatable({}, Animal)
    self.name = name
    return self
end

function Animal:speak()
    print(self.name .. " says something")
end

local a = Animal.new("Rex")
a:speak()  -- "Rex says something"
```

`a:speak()` 展开为 `a.speak(a)`。`a.speak` 通过 `__index` 找到 `Animal.speak`。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| Table | array + hash 混合 | 纯 hash |
| rehash | 计算最优 array/hash 分区 | 简单 2x 扩容 |
| #t | 二分搜索 O(log n) | 线性扫描 O(n) |
| UpVal | 一致 | 一致 |
| Metatable | 一致 | 一致 |
| 哈希函数 | 针对每种类型优化 | djb2 + 位运算 |
| 字符串键 | intern 去重 | 每次 strcmp |

## 测试

```
测试 01 (基础算术):     ✓
测试 02 (控制流):       ✓
测试 03 (函数/闭包):    ✓
测试 04 (table):        ✓  ← 新增
测试 05 (upvalue):      ✓  ← 新增
测试 06 (metatable):    ✓  ← 新增
```

全部 6 个测试通过。table 构造、索引访问、upvalue 闭包、metatable OOP 全部正确。
