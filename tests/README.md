# 测试用例

每个 `.lua` 文件是一个独立测试，`make test` 会依次运行。

## 命名约定

```
tests/
├── 01_basic.lua          # 阶段 1：基础算术与 print
├── 02_control.lua        # 阶段 1：if/while/for
├── 03_function.lua       # 阶段 1：函数与递归
├── 10_stack_vm.lua       # 阶段 2：字节码相关
├── 20_closure.lua        # 阶段 3：闭包与 upvalue
├── 21_table.lua          # 阶段 3：table 操作
├── 22_metatable.lua      # 阶段 3：元方法
├── 30_register_vm.lua    # 阶段 4：寄存器 VM
├── 40_gc.lua             # 阶段 5a：GC 压力
└── 50_coroutine.lua      # 阶段 5b：协程
```

## 对照官方测试

官方 Lua 5.1 测试套件位于 `test/` 目录，可挑选不依赖未实现特性的用例移植过来。