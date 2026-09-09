# 对照官方源码

本页系统对比 mini-lua 与 Lua 5.1.5 官方实现的差异——哪里偷了懒、哪里做了简化、哪里有不同选择。

## 对象系统

| 方面 | 官方 | 我们 | 简化原因 |
|------|------|------|---------|
| TValue | 有 `iscollectable()` 标记位 | 简单 tagged union | 教学简化 |
| 字符串 | 短字符串 intern 池（去重+快速比较） | 直接 `strdup` | 省去字符串池管理 |
| Table | **array + hash 混合** | **纯 hash**（开放寻址+线性探测） | 省去 rehash 分区策略 |
| 闭包 | LClosure + CClosure 两种 | 单一 Function（union） | 合并简化 |
| Proto | LineInfo、LocVars、source 等调试信息 | 只有 local_names | 省去调试信息 |

### Table 的 array+hash 混合

官方 Lua 的 Table 是最精妙的数据结构之一：

```text
// 官方
struct Table {
    TValue *array;  // 数组部分（整数键 1..n）
    Node *node;     // 哈希部分（其余键）
    int sizearray;  // 数组大小
    int lsizenode;  // 哈希大小（log2）
};
```

rehash 时计算最优分区：哪些键放数组、哪些放哈希。目标是最大化数组部分（缓存友好）。

我们纯哈希——所有键统一处理，省去分区逻辑。代价是整数键访问不如数组快。

## 指令集

官方 38 条，我们 30 条。

| 缺少的指令 | 官方用途 | 我们的替代 | 影响 |
|-----------|---------|-----------|------|
| `OP_SELF` | `obj:method()` 语法糖 | GETTABLE + CALL | 多一条指令 |
| `OP_TAILCALL` | 尾调用优化（复用当前帧） | 不支持 | 深递归可能栈溢出 |
| `OP_FORLOOP/OP_FORPREP` | 数值 for 专用指令 | while + 计数器 | 多几条指令 |
| `OP_TFORLOOP/OP_TFORPREP` | 泛型 for `for k,v in pairs(t)` | while + next | 多几条指令 |
| `OP_SETLIST` | table 批量构造 `{1,2,3}` | 逐条 SETTABLEK | 大 table 构造慢 |
| `OP_VARARG` | 可变参数 `...` | 不支持 | 无法用 `...` |
| `OP_TESTSET` | TEST + MOVE 合并 | TEST + MOVE | 多一条指令 |
| `OP_CLOSE` | 显式关闭 upvalue | RETURN 时统一关闭 | 语义一致 |

多出的指令：

| 指令 | 官方做法 | 我们的做法 |
|------|---------|-----------|
| `OP_NE` | NOT EQ | 独立指令 |
| `OP_GT` | NOT LE | 独立指令 |
| `OP_GE` | NOT LT | 独立指令 |
| `OP_SETTABLEK` | OP_SETTABLE + 常量 | 独立指令（常量键优化） |
| `OP_TESTN` | NOT TEST | 独立指令（or 短路） |

### 指令格式

| | 官方 | 我们 |
|---|------|------|
| 编码 | 32 位紧凑（Op:6 A:8 B:9 C:9） | struct OpCode, int A/B/C, int line |
| 大小 | 4 字节/指令 | ~20 字节/指令 |
| 优势 | 紧凑、缓存友好 | 直观、可读 |

