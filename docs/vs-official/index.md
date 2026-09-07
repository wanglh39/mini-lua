# 对照官方源码

本系列记录 mini-lua 与 Lua 5.1.5 官方实现的差异——哪里偷了懒、哪里做了简化、哪里有 bug。

## 对照维度

- **对象系统**：`TValue` / `GCObject` 的 tagged union 设计
- **table**：数组/哈希分区与 rehash 策略
- **upvalue**：open/closed 状态机实现
- **GC**：步进控制与写屏障
- **协程**：栈切换实现

> 随实现进度逐步填充。