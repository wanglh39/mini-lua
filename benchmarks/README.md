# 性能基准

每个 `.lua` 文件是一个基准用例，`make bench` 会依次运行并输出耗时。

## 基准用例

| 文件 | 考察点 | 对照官方 |
|------|--------|----------|
| `fib.lua` | 递归函数调用 | `fib(35)` |
| `ack.lua` | 深度递归 | `ack(3, 8)` |
| `table_rw.lua` | table 读写 | 大表遍历 |
| `string_concat.lua` | 字符串拼接 | 10000 次拼接 |
| `gc_pressure.lua` | GC 吞吐 | 大量分配/回收 |

## 运行

```bash
make bench                    # 运行所有基准
./mini-lua benchmarks/fib.lua # 运行单个
```

## 对照官方

```bash
lua5.1 benchmarks/fib.lua     # 官方 Lua 跑同一用例
```

记录对比结果到 `docs/benchmarks/`。