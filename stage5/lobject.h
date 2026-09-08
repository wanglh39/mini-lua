/*
 * lobject.h - 核心对象系统：TValue / GCObject / Table / Function / UpVal / Thread / Proto
 * 官方对照: lua-5.1.5/src/lobject.h
 * 实现阶段: 阶段 5（增量 GC + 协程）
 *
 * 阶段 5 新增：
 *   - GCObject 公共头：所有可回收对象（Table/Function/UpVal/Thread）以 GCObject 开头
 *   - mark-and-sweep GC：遍历根集标记可达对象，清除未标记对象
 *   - LUA_TTHREAD 类型 + lua_Thread 结构：协程（协作式线程）
 *   - setjmp/longjmp 实现 yield/resume
 */

#ifndef lobject_h
#define lobject_h


#include "llex.h"      /* TokenType（AST 的 binop 用） */
#include "lopcodes.h"  /* Instruction / OpCode（阶段 2 字节码） */

/* ─── 前向声明（打破循环依赖） ─── */
typedef struct lua_Value lua_Value;
typedef struct Function Function;
typedef struct Proto Proto;
typedef struct Table Table;
typedef struct UpVal UpVal;
typedef struct Exp Exp;
typedef struct ExpList ExpList;
typedef struct Stmt Stmt;
typedef struct Env Env;
typedef struct lua_Thread lua_Thread;
typedef struct GCObject GCObject;
typedef struct lua_State lua_State;  /* 定义在 lvm.h */

/* ─── GC 公共头 ─── */

/*
 * GCObject — 所有可回收对象的公共头。
 *
 * 每个可回收对象（Table/Function/UpVal/Thread）以 GCObject 开头，
 * 这样可以统一用 GCObject* 遍历所有对象，通过 gc_type 区分类型。
 *
 * marked: 0=白色(未标记), 1=灰色(待遍历), 2=黑色(已标记)
 * gc_type: LUA_TTABLE / LUA_TFUNCTION / LUA_TUPVAL / LUA_TTHREAD
 * next: 链表指针，串起所有 GC 对象
 */
struct GCObject {
    int marked;
    int gc_type;
    GCObject *next;
};

/* ─── 运行时值类型 ─── */

/*
 * Lua 值的类型标签。
 * 阶段 5 加入 LUA_TTHREAD（协程）。
 */
typedef enum {
    LUA_TNIL = 0,
    LUA_TBOOLEAN,
    LUA_TNUMBER,
    LUA_TSTRING,
    LUA_TFUNCTION,
    LUA_TTABLE,
    LUA_TTHREAD,
    LUA_TUPVAL,  /* 内部类型，不暴露给用户 */
} LuaType;

/*
 * 内置函数签名：C 函数，接收参数数组和参数个数，返回一个值。
 * 例如 print 就是一个 BuiltinFn。
 */
typedef lua_Value (*BuiltinFn)(lua_Value *args, int nargs);

/*
 * UpVal — upvalue 运行时表示。
 * 定义在 lua_Value 之后（value 字段需要完整类型），见下方。
 */

/*
 * 函数对象（闭包）。
 *
 * 分两种：
 *   1. 内置函数（is_builtin=1）：C 函数指针，如 print / type
 *   2. 用户函数（is_builtin=0）：Proto（字节码）+ UpVal 指针数组
 *
 * 闭包关键点：user.upvals 保存捕获的外层变量引用（UpVal*）。
 *   UpVal 是引用而非值拷贝——外层修改后闭包内能看到。
 */
struct Function {
    GCObject gc;     /* GC 头（必须第一个） */
    int is_builtin;
    char *name;
    union {
        BuiltinFn builtin;
        struct {
            Proto *proto;
            UpVal **upvals;  /* UpVal 指针数组（引用，非值拷贝） */
            int nupvals;
        } user;
    } u;
};

/*
 * Upvalue 描述——编译时记录每个 upvalue 指向外层的哪个变量。
 *
 * is_local=1：指向直接外层的第 index 个局部变量
 * is_local=0：指向直接外层的第 index 个 upvalue（隔层捕获）
 */
typedef struct {
    int is_local;
    int index;
} UpvalDesc;

/*
 * Proto——函数的"蓝图"（字节码 + 元数据）。
 *
 * 类比：Proto 是"类定义"，Function/Closure 是"实例"。
 * 同一段代码（如递归函数）只编译成一个 Proto，但每次执行可能创建多个闭包实例。
 *
 * code       — 指令数组
 * code_size  — 指令数量
 * constants  — 常量表（数字、字符串；OP_NUMBER/OP_STRING 用索引引用）
 * nconstants — 常量数量
 * local_names— 局部变量名表（调试用，按槽位索引）
 * nlocals    — 局部变量总数（含参数）
 * nparams    — 参数个数
 * subprotos  — 内嵌子函数的 Proto 数组（OP_CLOSURE 用索引引用）
 * nsubprotos — 子函数数量
 * upvals     — upvalue 描述数组
 * nupvals    — upvalue 数量
 */
struct Proto {
    Instruction *code;
    int code_size;
    lua_Value *constants;
    int nconstants;
    char **local_names;
    int nlocals;
    int nparams;
    int nregs;        /* 阶段 4：寄存器总数（>= nlocals） */
    Proto **subprotos;
    int nsubprotos;
    UpvalDesc *upvals;
    int nupvals;
};

/*
 * 运行时值——tagged union。
 *
 * type 标记当前 union 里存的是哪种值。
 * 阶段 1 简化：字符串直接用 char*（malloc 分配），不做 intern 去重。
 *   官方 Lua 区分短字符串（intern 到字符串池）和长字符串（独立分配），
 *   那是阶段 3/5a 的事。
 */
struct lua_Value {
    LuaType type;
    union {
        int b;           /* LUA_TBOOLEAN */
        double n;        /* LUA_TNUMBER  */
        char *s;         /* LUA_TSTRING  */
        Function *fn;    /* LUA_TFUNCTION */
        Table *t;        /* LUA_TTABLE    */
        lua_Thread *th;  /* LUA_TTHREAD   */
    } v;
};

/*
 * UpVal — upvalue 运行时表示。
 *
 * open 状态：ptr 指向调用帧的 regs 槽位（外层变量还活着）
 * closed 状态：ptr 指向自身 value（外层已返回，值已复制）
 *
 * 函数返回时 close 所有指向该帧 regs 的 open UpVal：
 *   把 *ptr 复制到 value，ptr 改为 &value。
 *
 * 这是 Lua 闭包实现的精髓：open/closed 状态机。
 */
struct UpVal {
    GCObject gc;     /* GC 头（必须第一个） */
    lua_Value *ptr;    /* open: 指向栈槽位; closed: 指向 &value */
    lua_Value value;   /* closed 时的值 */
    UpVal *next;       /* open upvalue 链表（VM 维护） */
};

/* ─── 协程（Thread） ─── */

/*
 * 协程状态。
 *   CO_SUSPENDED: 已创建或已 yield，等待 resume
 *   CO_RUNNING:   正在执行
 *   CO_DEAD:      已正常结束或出错
 *   CO_NORMAL:    已 resume 别的协程（被动的）
 */
typedef enum {
    CO_SUSPENDED = 0,
    CO_RUNNING,
    CO_DEAD,
    CO_NORMAL,
} CoStatus;

/*
 * lua_Thread — 协程（协作式线程）。
 *
 * 每个协程有自己的 lua_State（独立的帧栈/寄存器），
 * 但共享全局环境（globals）。
 *
 * yield/resume 机制（迭代式 VM + yield flag）：
 *   coroutine.yield(val) 设置 L->yield_requested=1, L->yield_value=val，返回。
 *   VM 主循环检测到 yield_requested，保存 yield_reg，立即返回 yield_value，不弹帧。
 *   coroutine.resume(val2) 调 resume_function：把 val2 写入 yield_reg 槽，继续主循环。
 *
 * 关键：协程的 VM 状态（帧栈/寄存器/ip）全在堆上（lua_State），
 *   yield 时帧栈原样保留，resume 时从断点继续。
 */
struct lua_Thread {
    GCObject gc;          /* GC 头（必须第一个） */
    struct lua_State *L;  /* 此协程的 VM 状态 */
    Function *fn;         /* 协程入口函数 */
    CoStatus status;      /* 协程状态 */
    int first_time;       /* 1=尚未启动, 0=已启动 */
    lua_Value yield_value;  /* yield 传给 resume 的值（冗余备份，主循环用 L->yield_value） */
    lua_Value resume_value; /* resume 传给 yield 的值 */
    lua_Thread *caller;   /* 谁 resume 了我 */
};



/* ─── AST 节点 ─── */

/*
 * 表达式节点类型。
 *
 * 阶段 1 支持的表达式：
 *   nil / true / false / 数字 / 字符串 / 变量引用
 *   二元运算 / 一元运算 / 函数调用 / 函数字面量
 *
 * 阶段 3 会加入 table 构造、索引访问、成员访问。
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
    EXP_TABLE,      /* table 构造：{key=val, ...} */
    EXP_INDEX,      /* 索引访问：a[key] 或 a.key */
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
        struct {               /* EXP_TABLE */
            struct TableEntry *entries;
            int nentries;
        } table;
        struct {               /* EXP_INDEX */
            Exp *obj;           /* table 表达式 */
            Exp *key;           /* 索引表达式 */
        } index;
    } u;
};

/* table 构造的条目：key 为 NULL 表示自动整数键（{1, 2, 3} 形式） */
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
 *   表达式语句 / local 声明 / 赋值 / if / while / for / return / 函数声明
 *
 * 注意：Lua 的"语句块"是语句序列，用 Stmt.next 链表表示，不需要单独的 block 类型。
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
    STMT_INDEX_ASSIGN, /* a[key] = value */
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
            Exp *obj;      /* table 表达式 */
            Exp *key;      /* 索引键表达式 */
            Exp *value;    /* 赋值表达式 */
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
lua_Value make_table(Table *t);
lua_Value make_thread(lua_Thread *th);

const char *type_name(LuaType type);   /* "nil" / "boolean" / ... */
void print_value(lua_Value v);         /* print 用的输出 */
int is_truthy(lua_Value v);            /* Lua 真值：nil 和 false 为假，其余为真 */

/* 值的相等判断（用于 == 运算） */
int value_equals(lua_Value a, lua_Value b);

#endif /* lobject_h */
