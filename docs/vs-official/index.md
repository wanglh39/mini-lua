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

