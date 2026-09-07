/*
 * lobject.h - 核心对象系统：TValue(tagged union) / Function / Env / AST 节点
 * 官方对照: lua-5.1.5/src/lobject.h
 * 实现阶段: 阶段 1（树遍历解释器）
 *
 * 阶段 1 与阶段 2+ 的关键区别：
 *   - 无 Proto / Instruction / OpCode（不生成字节码，直接遍历 AST 执行）
 *   - Function 的用户函数分支存 {params, body, env} 而非 {proto, upvals}
 *   - 闭包 = 函数体 + 定义时环境（env 链），变量查找沿 env->parent 链向上
 *   - 无 UpVal / UpvalDesc（env 链天然实现 upvalue 引用语义）
 *
 * AST 节点（Exp / Stmt）定义在此文件，因为阶段 1 需要建语法树。
 * 官方 Lua 单遍编译不建 AST，直接生成字节码；AST 是阶段 1 特有的。
 */

#ifndef lobject_h
#define lobject_h

#include "llex.h"      /* TokenType（AST 的 binop 用） */

/* ─── 前向声明（打破循环依赖） ─── */
typedef struct lua_Value lua_Value;
typedef struct Function Function;
typedef struct Exp Exp;
typedef struct ExpList ExpList;
typedef struct Stmt Stmt;
typedef struct Env Env;
typedef struct Binding Binding;

/* ─── 运行时值类型 ─── */

/*
 * Lua 值的类型标签。
 * 阶段 1 不支持 table（阶段 3 加入），保留 LUA_TTABLE 供 AST 类型兼容。
 */
typedef enum {
    LUA_TNIL = 0,
    LUA_TBOOLEAN,
    LUA_TNUMBER,
    LUA_TSTRING,
    LUA_TFUNCTION,
    LUA_TTABLE,  /* 保留供 AST 兼容，阶段 1 不创建 table 值 */
} LuaType;

/*
 * 内置函数签名：C 函数，接收参数数组和参数个数，返回一个值。
 * 例如 print 就是一个 BuiltinFn。
 */
typedef lua_Value (*BuiltinFn)(lua_Value *args, int nargs);

/*
 * 运行时值——tagged union。
 *
 * type 标记当前 union 里存的是哪种值。
 * 阶段 1 简化：字符串直接用 char*（malloc 分配），不做 intern 去重。
 */
struct lua_Value {
    LuaType type;
    union {
        int b;           /* LUA_TBOOLEAN */
        double n;        /* LUA_TNUMBER  */
        char *s;         /* LUA_TSTRING  */
        Function *fn;    /* LUA_TFUNCTION */
    } v;
};

/*
 * Env — 环境（变量绑定链表 + 父环境指针）。
 *
 * 树遍历解释器的核心数据结构：
 *   - bindings：本层变量绑定（链表）
 *   - parent：外层环境（全局 → 函数 → 块作用域）
 *
 * 变量查找：沿 parent 链向上搜索，找到第一个匹配的绑定。
 * 闭包：创建时捕获当前 Env 指针，调用时以此 Env 为 parent 创建新 Env。
 *   这天然实现了 upvalue 引用语义——外层修改后闭包内能看到，因为共享同一 Env。
 */
typedef struct Binding {
    char *name;
    lua_Value value;
    Binding *next;
} Binding;

struct Env {
    Binding *bindings;
    Env *parent;
};

/*
 * 函数对象（闭包）。
 *
 * 分两种：
 *   1. 内置函数（is_builtin=1）：C 函数指针，如 print / type
 *   2. 用户函数（is_builtin=0）：参数列表 + 函数体 AST + 定义时环境
 *
 * 闭包关键点：user.env 保存定义时的环境引用。
 *   调用时以此 env 为 parent 创建新 env，绑定参数后执行 body。
 *   由于 env 是引用（非拷贝），外层变量修改后闭包内能看到（upvalue 语义）。
 *
 * 对比阶段 2+：
 *   阶段 2+ 用 Proto（字节码）+ UpVal（open/closed 状态机）实现闭包。
 *   阶段 1 用 env 链直接引用，更简单但无法 close upvalue（env 一直活着）。
 */
struct Function {
    int is_builtin;
    char *name;
    union {
        BuiltinFn builtin;
        struct {
            char **params;
            int nparams;
            Stmt *body;
            Env *env;       /* 定义时捕获的环境 */
        } user;
    } u;
};

/* ─── AST 节点 ─── */

/*
 * 表达式节点类型。
 *
 * 阶段 1 支持的表达式：
 *   nil / true / false / 数字 / 字符串 / 变量引用
 *   二元运算 / 一元运算 / 函数调用 / 函数字面量
 *
 * EXP_TABLE / EXP_INDEX 保留供 parser 使用，阶段 1 求值时报错。
 */
typedef enum {
    EXP_NIL,
    EXP_BOOL,
    EXP_NUMBER,
    EXP_STRING,
    EXP_VAR,        /* 变量引用：名字 */
    EXP_BINOP,      /* 二元运算：a + b */
    EXP_UNOP,       /* 一元运算：-a, not a */
    EXP_CALL,       /* 函数调用：f(args) */
    EXP_FUNCTION,   /* 函数字面量：function(params) body end */
    EXP_TABLE,      /* table 构造（阶段 1 不支持，报错） */
    EXP_INDEX,      /* 索引访问（阶段 1 不支持，报错） */
} ExpType;

/*
 * 表达式 AST 节点。
 *
 * 每个节点记录 line（错误信息用）和 type（区分 union 哪个分支）。
 * union u 的各分支对应 ExpType 各类型。
 */
struct Exp {
    ExpType type;
    int line;
    union {
        int boolean;           /* EXP_BOOL   */
        double number;         /* EXP_NUMBER */
        char *string;          /* EXP_STRING */
        char *var;             /* EXP_VAR    */
        struct {               /* EXP_BINOP  */
            TokenType op;
            Exp *left, *right;
        } binop;
        struct {               /* EXP_UNOP   */
            TokenType op;      /* TK_MINUS / TK_NOT / TK_LEN */
            Exp *operand;
        } unop;
        struct {               /* EXP_CALL   */
            Exp *fn;           /* 被调用的函数表达式 */
            ExpList *args;     /* 实参列表 */
        } call;
        struct {               /* EXP_FUNCTION */
            char **params;
            int nparams;
            Stmt *body;
        } func;
        struct {               /* EXP_TABLE（阶段 1 不支持） */
            struct TableEntry *entries;
            int nentries;
        } table;
        struct {               /* EXP_INDEX（阶段 1 不支持） */
            Exp *obj;
            Exp *key;
        } index;
    } u;
};

/* table 构造的条目（保留供 parser 使用） */
typedef struct TableEntry {
    Exp *key;
    Exp *value;
} TableEntry;

/* 表达式列表（函数调用的实参、return 的返回值列表） */
struct ExpList {
    Exp *head;
    ExpList *tail;  /* NULL 表示列表结束 */
};

/*
 * 语句节点类型。
 *
 * 阶段 1 支持的语句：
 *   表达式语句 / local 声明 / 赋值 / if / while / for / return / 函数声明 / break
 *
 * STMT_INDEX_ASSIGN 保留供 parser 使用，阶段 1 执行时报错。
 */
typedef enum {
    STMT_EXPR,      /* 表达式语句：foo() */
    STMT_LOCAL,     /* local name = value */
    STMT_ASSIGN,    /* name = value */
    STMT_IF,        /* if cond then ... else ... end */
    STMT_WHILE,     /* while cond do ... end */
    STMT_FOR,       /* for var = start, end[, step] do ... end */
    STMT_RETURN,    /* return value */
    STMT_FUNCTION,  /* function name(params) body end */
    STMT_BREAK,     /* break */
    STMT_INDEX_ASSIGN, /* a[key] = value（阶段 1 不支持，报错） */
} StmtType;

/*
 * 语句 AST 节点。
 *
 * next 指向下一条语句，形成链表（即"语句块"）。
 */
struct Stmt {
    StmtType type;
    int line;
    union {
        Exp *expr;                                          /* STMT_EXPR */
        struct { char *name; Exp *value; } local;            /* STMT_LOCAL  */
        struct { char *name; Exp *value; } assign;           /* STMT_ASSIGN */
        struct {                                              /* STMT_IF */
            Exp *cond;
            Stmt *then_branch;  /* then 块的语句链表头 */
            Stmt *else_branch;  /* else 块，可为 NULL */
        } if_;
        struct { Exp *cond; Stmt *body; } while_;            /* STMT_WHILE */
        struct {                                              /* STMT_FOR */
            char *var;
            Exp *start, *end, *step;  /* step 可为 NULL（默认 1） */
            Stmt *body;
        } for_;
        Exp *return_value;                                    /* STMT_RETURN */
        struct {                                              /* STMT_FUNCTION */
            char *name;
            char **params;
            int nparams;
            Stmt *body;
        } func;
        struct {                                              /* STMT_INDEX_ASSIGN */
            Exp *obj;
            Exp *key;
            Exp *value;
        } index_assign;
        /* STMT_BREAK 无额外字段 */
    } u;
    Stmt *next;  /* 语句链表：下一条语句，NULL 表示结束 */
};

/* ─── 值操作辅助函数 ─── */

lua_Value make_nil(void);
lua_Value make_boolean(int b);
lua_Value make_number(double n);
lua_Value make_string(const char *s);  /* 复制字符串 */
lua_Value make_string_owned(char *s);  /* 接管已有 malloc 字符串 */
lua_Value make_function(Function *fn);

const char *type_name(LuaType type);   /* "nil" / "boolean" / ... */
void print_value(lua_Value v);         /* print 用的输出 */
int is_truthy(lua_Value v);            /* Lua 真值：nil 和 false 为假，其余为真 */

/* 值的相等判断（用于 == 运算） */
int value_equals(lua_Value a, lua_Value b);

/* ─── 环境操作 ─── */

Env *env_create(Env *parent);
void env_define(Env *env, const char *name, lua_Value value);
lua_Value *env_lookup(Env *env, const char *name);
void env_assign(Env *env, const char *name, lua_Value value);

#endif /* lobject_h */