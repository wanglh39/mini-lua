# 阶段 1：树遍历解释器

> 源码目录：`stage1/`
> 官方对照：Lua 5.1.5 无直接对应（官方一开始就是字节码 VM）
> 测试：01-03 通过，04-06 报"阶段 1 不支持 table"

## 目标

不编译成字节码，直接递归遍历 AST 执行。理解 Lua 语法和运行时的最小内核。

## 架构

```
源码 → llex 词法 → lparser 语法(AST) → lvm 树遍历执行
```

没有 lcode.c（编译器）、lopcodes.h（指令集）。lvm.c 的核心是 `eval_exp` / `exec_stmt` 两个递归函数。

## 核心实现

### 闭包 = {body, env}

```c
struct Function {
    char **params;
    int nparams;
    Stmt *body;
    Env *env;  // 定义时的环境
};
```

闭包捕获的是**环境引用**（Env*），不是值拷贝。外层变量修改后闭包内能看到——因为 `env_lookup` 沿着 `parent` 链查找。

### 环境 = 绑定链表

```c
struct Env {
    Binding *bindings;  // name → value 链表
    Env *parent;        // 外层环境
};
```

`call_function` 创建新 env（parent = 闭包的 env），绑定参数，执行 body。

## 关键决策

### 为什么保留 AST 类型但不支持 table？

`lobject.h` 保留了 `EXP_TABLE`、`EXP_INDEX`、`STMT_INDEX_ASSIGN` 等 AST 节点类型，因为 `lparser.c` 依赖它们。阶段 1 的 `eval_exp` 遇到这些类型直接报错"阶段 1 不支持 table"。

这样 llex.c / lparser.c 可以原封不动复用到后续阶段。

### 结构体顺序

`lua_Value` 必须在 `Binding` 之前完整定义，因为 `Binding` 包含 `lua_Value value` 字段。前向声明不够——C 的 struct 成员需要完整类型。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| 执行方式 | 字节码 VM | 树遍历 |
| 闭包 | Proto + UpVal[] | {body, env} |
| 变量查找 | 寄存器/UpVal（O(1)） | 环境链表遍历（O(n)） |

树遍历解释器性能差（变量查找 O(n)），但实现简单，适合理解语义。