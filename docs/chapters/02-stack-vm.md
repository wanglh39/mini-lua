# 阶段 2：栈式字节码 VM

> 源码目录：`stage2/`
> 官方对照：lua-5.1.5/src/lvm.c（栈式部分）
> 测试：01-03 通过，04-06 报"阶段 2 不支持 table"

## 目标

引入编译器（lcode.c）和指令集（lopcodes.h），生成字节码再执行。理解编译和执行分离的好处。

## 架构

```
源码 → llex → lparser(AST) → lcode(字节码) → lvm 栈式 VM 执行
```

## 栈式 VM

每条指令从值栈顶取操作数，结果压回栈顶：

```
ADD → 弹出 b, 弹出 a, 压入 a+b
```

指令集：`OP_NIL`、`OP_NUMBER`、`OP_GETLOCAL`、`OP_SETLOCAL`、`OP_ADD`、`OP_JMP`（绝对跳转）等。

## 关键决策

### 绝对跳转 vs 相对跳转

阶段 2 用绝对跳转（`OP_JMP target`，target 是目标指令索引）。
阶段 4 改为相对跳转（`OP_JMP offset`，offset 相对当前 PC）——更适合代码移动和闭包。

### 不支持 table

从 stage3 删除 ltable.c/ltm.c/ltablib.c，并在 lcode.c 中让 `EXP_TABLE`/`EXP_INDEX`/`STMT_INDEX_ASSIGN` 报错。

## 对照官方

Lua 5.0 及之前是栈式 VM，5.1 起改为寄存器式。我们阶段 2→4 复现了这个演进。