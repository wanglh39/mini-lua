# 阶段 1：树遍历解释器

> 源码目录：`stage1/`
> 官方对照：Lua 5.1.5 无直接对应（官方一开始就是字节码 VM）
> 测试：01-03 通过，04-06 报"阶段 1 不支持 table"

## 目标

不编译成字节码，直接递归遍历 AST 执行。理解 Lua 语法和运行时的最小内核。

这是整个项目的起点。我们不走官方 Lua 的路线（一上来就是字节码 VM），而是先写一个最简单的树遍历解释器——就像 Python 早期或 Ruby 那样，解析出 AST 后直接遍历执行。

好处是：**把"理解语义"和"实现执行引擎"解耦**。阶段 1 只管语义对不对，不管执行效率。等语义对了，阶段 2 再换成字节码 VM，执行逻辑完全重写，但语法分析器不动。

## 架构

```
源码 → llex 词法 → lparser 语法(AST) → lvm 树遍历执行
```

没有 lcode.c（编译器）、lopcodes.h（指令集）。lvm.c 的核心是 `eval_exp` / `exec_stmt` 两个递归函数。

### 与后续阶段的文件复用

| 文件 | 阶段 1 | 阶段 2+ |
|------|--------|---------|
| llex.h/c | 完整复用 | 完整复用 |
| lparser.h/c | 完整复用 | 完整复用 |
| lobject.h | **不同**（有 AST + Env，无 Proto/Instruction） | 有 Proto/Instruction |
| lvm.h/c | **不同**（eval_exp/exec_stmt） | call_function + VM 主循环 |
| lcode.h/c | **不存在** | 编译器 |
| lopcodes.h | **不存在** | 指令集 |

关键设计：llex 和 lparser 在所有阶段都不变。变化的是"AST 之后怎么处理"——阶段 1 直接执行，阶段 2+ 编译成字节码再执行。

## 词法分析 (llex.c)

词法分析器把源码字符串切成 Token 序列。Lua 的 Token 相对简单：

```c
typedef enum {
    TK_NUMBER, TK_STRING, TK_NAME,
    TK_LOCAL, TK_FUNCTION, TK_END, TK_IF, TK_THEN, TK_ELSE, TK_ELSEIF,
    TK_WHILE, TK_DO, TK_FOR, TK_RETURN, TK_BREAK, TK_NIL, TK_TRUE, TK_FALSE,
    TK_AND, TK_OR, TK_NOT, TK_IN,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_CARET, TK_HASH,
    TK_EQ, TK_NE, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_ASSIGN, TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET, TK_SEMICOLON, TK_COMMA, TK_COLON, TK_DOT,
    TK_CONCAT, TK_EOF,
} TokenType;
```

### 关键实现

```c
// 跳过空白和注释
static void skip_whitespace(Lexer *lx);

// 读取数字：支持整数和小数
static Token read_number(Lexer *lx);

// 读取字符串：处理转义序列
static Token read_string(Lexer *lx);

// 读取标识符：判断是关键字还是变量名
static Token read_name(Lexer *lx);
```

Lua 的注释有两种：单行 `--` 和多行 `--[[ ... ]]`。词法分析器需要正确处理嵌套的多行注释 `--[==[ ... ]==]`。

## 语法分析 (lparser.c)

递归下降解析器，直接手写不用 yacc/bison。每个语法产生式对应一个函数：

```c
Stmt *parse_program(Lexer *lx);      // 入口
static Stmt *parse_block(Lexer *lx); // 语句块
static Stmt *parse_stmt(Lexer *lx);  // 单条语句
static Exp *parse_exp(Lexer *lx);    // 表达式（优先级爬升）
```

### AST 节点

```c
// 表达式
typedef enum {
    EXP_NIL, EXP_BOOL, EXP_NUMBER, EXP_STRING,
    EXP_VAR,        // 变量引用
    EXP_BINOP,      // 二元运算
    EXP_UNOP,       // 一元运算
    EXP_CALL,       // 函数调用
    EXP_FUNCTION,   // 函数字面量
    EXP_TABLE,      // table 构造
    EXP_INDEX,      // 索引访问
} ExpType;

// 语句
typedef enum {
    STMT_EXPR,      // 表达式语句
    STMT_LOCAL,     // local name = value
    STMT_ASSIGN,    // name = value
    STMT_IF,        // if cond then ... else ... end
    STMT_WHILE,     // while cond do ... end
    STMT_FOR,       // for var = start, end[, step] do ... end
    STMT_RETURN,    // return value
    STMT_FUNCTION,  // function name(params) body end
    STMT_BREAK,     // break
    STMT_INDEX_ASSIGN, // a[key] = value
} StmtType;
```

### 运算符优先级

Lua 的运算符优先级（从低到高）：

```
or
and
<  >  <=  >=  ~=  ==
..  (右结合)
+  -
*  /  %
unary (not  #  -)
^    (右结合)
```

用优先级爬升法（precedence climbing）解析：

```c
static Exp *parse_binop_rhs(Lexer *lx, int min_prec, Exp *lhs) {
    while (current_prec(lx) >= min_prec) {
        TokenType op = advance(lx);
        Exp *rhs = parse_unary(lx);
        int next_prec = current_prec(lx);
        if (next_prec > prec(op) || (right_assoc(op) && next_prec == prec(op)))
            rhs = parse_binop_rhs(lx, next_prec, rhs);
        lhs = make_binop(op, lhs, rhs);
    }
    return lhs;
}
```

## 树遍历执行 (lvm.c)

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

struct Binding {
    char *name;
    lua_Value value;
    Binding *next;
};
```

`call_function` 创建新 env（parent = 闭包的 env），绑定参数，执行 body。

### eval_exp — 表达式求值

```c
lua_Value eval_exp(Exp *e, Env *env) {
    switch (e->type) {
        case EXP_NUMBER: return make_number(e->u.number);
        case EXP_STRING: return make_string(e->u.string);
        case EXP_VAR: {
            lua_Value *slot = env_lookup(env, e->u.var);
            return slot ? *slot : make_nil();
        }
        case EXP_BINOP: {
            lua_Value l = eval_exp(e->u.binop.left, env);
            lua_Value r = eval_exp(e->u.binop.right, env);
            return do_binop(e->u.binop.op, l, r);
        }
        case EXP_CALL: {
            lua_Value fn = eval_exp(e->u.call.fn, env);
            lua_Value args[16]; int nargs = 0;
            for (ExpList *el = e->u.call.args; el; el = el->tail)
                args[nargs++] = eval_exp(el->head, env);
            return call_function(fn.v.fn, args, nargs);
        }
        case EXP_FUNCTION:
            return make_function(create_closure(e, env));
        // ...
    }
}
```

### exec_stmt — 语句执行

```c
int exec_stmt(Stmt *s, Env *env) {
    switch (s->type) {
        case STMT_LOCAL:
            env_define(env, s->u.local.name, eval_exp(s->u.local.value, env));
            return CONTROL_NORMAL;
        case STMT_ASSIGN: {
            lua_Value val = eval_exp(s->u.assign.value, env);
            env_set(env, s->u.assign.name, val);
            return CONTROL_NORMAL;
        }
        case STMT_IF: {
            lua_Value cond = eval_exp(s->u.if_.cond, env);
            if (is_truthy(cond))
                return exec_block(s->u.if_.then_branch, env);
            else if (s->u.if_.else_branch)
                return exec_block(s->u.if_.else_branch, env);
            return CONTROL_NORMAL;
        }
        case STMT_RETURN:
            env_define(env, "__return", eval_exp(s->u.return_value, env));
            return CONTROL_RETURN;
        case STMT_BREAK:
            return CONTROL_BREAK;
        // ...
    }
}
```

### 控制流传播

`exec_block` 遍历语句链表，检查控制流标志：

```c
int exec_block(Stmt *block, Env *env) {
    for (Stmt *s = block; s; s = s->next) {
        int ctrl = exec_stmt(s, env);
        if (ctrl != CONTROL_NORMAL) return ctrl;
    }
    return CONTROL_NORMAL;
}
```

`CONTROL_NORMAL`、`CONTROL_RETURN`、`CONTROL_BREAK` 三个值表示语句执行后的控制流状态。`while` 循环检查 `CONTROL_BREAK` 来跳出，`call_function` 检查 `CONTROL_RETURN` 来返回值。

## 关键决策

### 为什么保留 AST 类型但不支持 table？

`lobject.h` 保留了 `EXP_TABLE`、`EXP_INDEX`、`STMT_INDEX_ASSIGN` 等 AST 节点类型，因为 `lparser.c` 依赖它们。阶段 1 的 `eval_exp` 遇到这些类型直接报错：

```c
case EXP_TABLE:
    fprintf(stderr, "阶段 1 不支持 table\n");
    exit(1);
```

这样 llex.c / lparser.c 可以原封不动复用到后续阶段——不需要 `#ifdef STAGE1` 条件编译。

### 结构体顺序

`lua_Value` 必须在 `Binding` 之前完整定义，因为 `Binding` 包含 `lua_Value value` 字段。C 的 struct 成员需要完整类型，前向声明不够。

正确的顺序：
```c
struct lua_Value { ... };  // 先定义
struct Binding { lua_Value value; ... };  // 再定义
struct Env { Binding *bindings; ... };  // 最后定义
```

## 遇到的问题

### 问题 1：闭包捕获了错误的变量

最初 `create_closure` 直接把 `env` 存入闭包。但如果 `env` 是函数调用的临时环境，函数返回后 env 被释放，闭包就悬空了。

**修复**：闭包捕获的 env 必须是**定义时的外层 env**，不是调用时的 env。在 `eval_exp` 的 `EXP_FUNCTION` 分支中，传入的 `env` 就是定义时的环境，直接存入闭包即可。

### 问题 2：local 声明的值在循环中重复绑定

`for` 循环每次迭代调用 `env_define` 创建新 binding，导致环境链表越来越长。

**修复**：`for` 循环体使用同一个 env，每次迭代只更新已有 binding 的值，不创建新 binding。

## 对照官方

| 方面 | 官方 | 我们 |
|------|------|------|
| 执行方式 | 字节码 VM | 树遍历 |
| 闭包 | Proto + UpVal[] | {body, env} |
| 变量查找 | 寄存器/UpVal（O(1)） | 环境链表遍历（O(n)） |
| 内存 | GC 管理 | malloc，不回收 |
| 错误处理 | longjmp + pcall | exit(1) |

树遍历解释器性能差（变量查找 O(n)），但实现简单，适合理解语义。这是后续所有阶段的基础——语法和语义对了，执行引擎可以随时换。

## 测试

```
测试 01 (基础算术):     ✓
测试 02 (控制流):       ✓
测试 03 (函数/闭包):    ✓
测试 04 (table):        ✗ "阶段 1 不支持 table"
测试 05 (upvalue):      ✗
测试 06 (metatable):    ✗
```

前三个测试通过说明：词法分析、语法分析、基本运算、控制流、函数调用、闭包语义全部正确。table 相关功能留给阶段 3。
