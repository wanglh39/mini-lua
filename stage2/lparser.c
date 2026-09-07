/*
 * lparser.c - 语法分析实现（递归下降、单遍编译直接生成字节码）
 * 官方对照: lua-5.1.5/src/lparser.c
 * 实现阶段: 阶段 1
 *
 * 递归下降解析器，边读 token 边建 AST。
 *
 * 表达式解析用"优先级爬升法"（precedence climbing）：
 *   每个二元运算符有优先级，解析时根据优先级决定是否继续吃进更高级的运算符。
 *   这比"为每个优先级写一个函数"更紧凑，也是官方 Lua 的做法。
 *
 * 语句解析用 switch 分发：看当前 token 是 local/if/while/for/function/return/name 就走对应分支。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lparser.h"

/* ─── AST 节点创建辅助 ─── */

static Exp *new_exp(ExpType type, int line) {
    Exp *e = (Exp *)calloc(1, sizeof(Exp));
    e->type = type;
    e->line = line;
    return e;
}

static Exp *new_exp_nil(int line)       { return new_exp(EXP_NIL, line); }
static Exp *new_exp_bool(int b, int l)  { Exp *e = new_exp(EXP_BOOL, l); e->u.boolean = b; return e; }
static Exp *new_exp_number(double n, int l) { Exp *e = new_exp(EXP_NUMBER, l); e->u.number = n; return e; }
static Exp *new_exp_string(char *s, int l)  { Exp *e = new_exp(EXP_STRING, l); e->u.string = s; return e; }
static Exp *new_exp_var(char *name, int l)  { Exp *e = new_exp(EXP_VAR, l); e->u.var = name; return e; }

static Exp *new_exp_binop(TokenType op, Exp *left, Exp *right, int line) {
    Exp *e = new_exp(EXP_BINOP, line);
    e->u.binop.op = op;
    e->u.binop.left = left;
    e->u.binop.right = right;
    return e;
}

static Exp *new_exp_unop(TokenType op, Exp *operand, int line) {
    Exp *e = new_exp(EXP_UNOP, line);
    e->u.unop.op = op;
    e->u.unop.operand = operand;
    return e;
}

static Exp *new_exp_call(Exp *fn, ExpList *args, int line) {
    Exp *e = new_exp(EXP_CALL, line);
    e->u.call.fn = fn;
    e->u.call.args = args;
    return e;
}

static Exp *new_exp_function(char **params, int nparams, Stmt *body, int line) {
    Exp *e = new_exp(EXP_FUNCTION, line);
    e->u.func.params = params;
    e->u.func.nparams = nparams;
    e->u.func.body = body;
    return e;
}

/* table 构造：{entries} */
static Exp *new_exp_table(TableEntry *entries, int nentries, int line) {
    Exp *e = new_exp(EXP_TABLE, line);
    e->u.table.entries = entries;
    e->u.table.nentries = nentries;
    return e;
}

/* 索引访问：obj[key] */
static Exp *new_exp_index(Exp *obj, Exp *key, int line) {
    Exp *e = new_exp(EXP_INDEX, line);
    e->u.index.obj = obj;
    e->u.index.key = key;
    return e;
}

static ExpList *new_exp_list(Exp *head, ExpList *tail) {
    ExpList *l = (ExpList *)malloc(sizeof(ExpList));
    l->head = head;
    l->tail = tail;
    return l;
}

static Stmt *new_stmt(StmtType type, int line) {
    Stmt *s = (Stmt *)calloc(1, sizeof(Stmt));
    s->type = type;
    s->line = line;
    return s;
}

/* ─── token 辅助 ─── */

/* 期望当前 token 是指定类型，否则报错并退出 */
static void expect(Parser *p, TokenType type, const char *what) {
    Token t = lex_current(&p->lex);
    if (t.type != type) {
        fprintf(stderr, "语法错误（第 %d 行）：期望 %s，但遇到 %s\n",
                t.line, what, token_to_string(t.type));
        exit(1);
    }
    lex_advance(&p->lex);
}

/* 如果当前 token 是指定类型，消费并返回 1，否则返回 0 */
static int accept(Parser *p, TokenType type) {
    if (lex_current(&p->lex).type == type) {
        lex_advance(&p->lex);
        return 1;
    }
    return 0;
}

/* 消费当前 token，返回其字符串值（用于读标识符名） */
static char *expect_name(Parser *p) {
    Token t = lex_current(&p->lex);
    if (t.type != TK_NAME) {
        fprintf(stderr, "语法错误（第 %d 行）：期望标识符，但遇到 %s\n",
                t.line, token_to_string(t.type));
        exit(1);
    }
    char *name = strdup(t.value.s);
    lex_advance(&p->lex);
    return name;
}

/* ─── 表达式解析 ─── */

/* 二元运算符优先级（数值越大优先级越高，0 表示不是二元运算符） */
static int binop_priority(TokenType op) {
    switch (op) {
        case TK_OR:          return 1;  /* or */
        case TK_AND:         return 2;  /* and */
        case TK_LT: case TK_GT: case TK_LE: case TK_GE:
        case TK_NE: case TK_EQ:        return 3;  /* 比较运算 */
        case TK_CONCAT:      return 4;  /* .. （右结合） */
        case TK_PLUS: case TK_MINUS:   return 5;  /* + - */
        case TK_STAR: case TK_SLASH: case TK_PERCENT:
                             return 6;  /* * / % */
        case TK_CARET:       return 7;  /* ^ （右结合） */
        default:             return 0;  /* 不是二元运算符 */
    }
}

/* 是否右结合运算符（.. 和 ^ 是右结合） */
static int is_right_assoc(TokenType op) {
    return op == TK_CONCAT || op == TK_CARET;
}

/* 前向声明 */
static Exp *parse_exp(Parser *p);
static Stmt *parse_block(Parser *p);

/*
 * 解析 table 构造器：{ field {, field} [,] }
 *
 * field 有三种形式：
 *   [exp] = exp   — 计算键（方括号）
 *   Name = exp    — 名字作字符串键（record 风格）
 *   exp           — 自动整数键（array 风格，键从 1 递增）
 *
 * 例：{} / {1,2,3} / {x=1,y=2} / {["x"]=1} / {1, x=2, [3]=4}
 *
 * 判断 Name=exp 还是 exp：
 *   先解析表达式 e，若后面跟 '=' 则 e 必须是 EXP_VAR（名字），
 *   转为字符串键；否则 e 是自动整数键的值。
 */
static Exp *parse_table_constr(Parser *p, int line) {
    expect(p, TK_LBRACE, "'{'");
    TableEntry *entries = NULL;
    int nentries = 0, cap = 0;
    int auto_idx = 1;  /* 自动整数键的下一个值 */

    while (lex_current(&p->lex).type != TK_RBRACE &&
           lex_current(&p->lex).type != TK_EOF) {
        if (nentries >= cap) {
            cap = cap ? cap * 2 : 8;
            entries = (TableEntry *)realloc(entries, cap * sizeof(TableEntry));
        }

        Token t = lex_current(&p->lex);
        Exp *key = NULL;
        Exp *value;

        if (t.type == TK_LBRACKET) {
            /* [exp] = exp — 计算键 */
            lex_advance(&p->lex);
            key = parse_exp(p);
            expect(p, TK_RBRACKET, "']'");
            expect(p, TK_ASSIGN, "'='");
            value = parse_exp(p);
        } else {
            /* 先解析表达式，再判断后面是否跟 '=' */
            Exp *e = parse_exp(p);
            if (accept(p, TK_ASSIGN)) {
                /* Name = exp：e 必须是变量名 */
                if (e->type != EXP_VAR) {
                    fprintf(stderr, "语法错误（第 %d 行）：table record 键必须是名字\n", t.line);
                    exit(1);
                }
                key = new_exp_string(e->u.var, t.line);
                value = parse_exp(p);
            } else {
                /* 自动整数键 */
                key = new_exp_number(auto_idx, t.line);
                value = e;
            }
        }

        entries[nentries].key = key;
        entries[nentries].value = value;
        nentries++;
        auto_idx++;

        /* 字段分隔符：逗号或分号（Lua 允许末尾分隔符） */
        if (!accept(p, TK_COMMA) && !accept(p, TK_SEMICOLON))
            break;
    }
    expect(p, TK_RBRACE, "'}'");
    return new_exp_table(entries, nentries, line);
}

/* 解析基本表达式：字面量、变量、函数字面量、括号、函数调用 */
static Exp *parse_primary(Parser *p) {
    Token t = lex_current(&p->lex);
    int line = t.line;

    switch (t.type) {
        case TK_NIL:
            lex_advance(&p->lex);
            return new_exp_nil(line);

        case TK_TRUE:
            lex_advance(&p->lex);
            return new_exp_bool(1, line);

        case TK_FALSE:
            lex_advance(&p->lex);
            return new_exp_bool(0, line);

        case TK_NUMBER:
            lex_advance(&p->lex);
            return new_exp_number(t.value.n, line);

        case TK_STRING: {
            /* 先复制字符串，lex_advance 会释放原 token 里的字符串 */
            char *s = strdup(t.value.s);
            lex_advance(&p->lex);
            return new_exp_string(s, line);
        }

        case TK_NAME: {
            /* 标识符：可能是变量引用，也可能是函数调用 */
            char *name = strdup(t.value.s);
            lex_advance(&p->lex);
            return new_exp_var(name, line);
        }

        case TK_LPAREN: {
            /* 括号表达式：( exp ) */
            lex_advance(&p->lex);
            Exp *e = parse_exp(p);
            expect(p, TK_RPAREN, "')'");
            return e;
        }

        case TK_LBRACE:
            /* table 构造器：{ ... } */
            return parse_table_constr(p, line);

        case TK_FUNCTION: {
            /* 函数字面量：function ( params ) block end */
            lex_advance(&p->lex);
            expect(p, TK_LPAREN, "'('");
            /* 解析参数列表 */
            char **params = NULL;
            int nparams = 0, cap = 0;
            if (lex_current(&p->lex).type != TK_RPAREN) {
                do {
                    char *pname = expect_name(p);
                    if (nparams >= cap) { cap = cap ? cap * 2 : 4; params = (char **)realloc(params, cap * sizeof(char *)); }
                    params[nparams++] = pname;
                } while (accept(p, TK_COMMA));
            }
            expect(p, TK_RPAREN, "')'");
            Stmt *body = parse_block(p);
            expect(p, TK_END, "'end'");
            return new_exp_function(params, nparams, body, line);
        }

        default:
            fprintf(stderr, "语法错误（第 %d 行）：意外的符号 %s\n",
                    line, token_to_string(t.type));
            exit(1);
    }
}

/*
 * 解析后缀操作：函数调用 / 索引访问 / 成员访问。
 *
 * primary 返回一个表达式，后面可能跟：
 *   (args)  — 函数调用
 *   [exp]   — 索引访问
 *   .Name   — 成员访问（等价于 ["Name"]）
 *
 * 这些后缀可以链式叠加：a.b[c](x) → 先 .b，再 [c]，再 (x)。
 */
static Exp *parse_suffix(Parser *p) {
    Exp *e = parse_primary(p);
    for (;;) {
        Token t = lex_current(&p->lex);
        int line = t.line;
        if (t.type == TK_LPAREN) {
            /* 函数调用：e ( args ) */
            lex_advance(&p->lex);
            ExpList *args = NULL;
            ExpList **tail = &args;
            if (lex_current(&p->lex).type != TK_RPAREN) {
                do {
                    Exp *arg = parse_exp(p);
                    *tail = new_exp_list(arg, NULL);
                    tail = &(*tail)->tail;
                } while (accept(p, TK_COMMA));
            }
            expect(p, TK_RPAREN, "')'");
            e = new_exp_call(e, args, line);
        } else if (t.type == TK_LBRACKET) {
            /* 索引访问：e [ exp ] */
            lex_advance(&p->lex);
            Exp *key = parse_exp(p);
            expect(p, TK_RBRACKET, "']'");
            e = new_exp_index(e, key, line);
        } else if (t.type == TK_DOT) {
            /* 成员访问：e . Name */
            lex_advance(&p->lex);
            char *name = expect_name(p);
            Exp *key = new_exp_string(name, line);
            e = new_exp_index(e, key, line);
        } else {
            break;  /* 不是后缀操作，结束 */
        }
    }
    return e;
}

/* 解析一元前缀运算：-exp, not exp, #exp */
static Exp *parse_unop(Parser *p) {
    Token t = lex_current(&p->lex);
    if (t.type == TK_MINUS || t.type == TK_NOT || t.type == TK_LEN) {
        int line = t.line;
        lex_advance(&p->lex);
        Exp *operand = parse_unop(p);  /* 一元运算右结合：--a => -(−a) */
        return new_exp_unop(t.type, operand, line);
    }
    return parse_suffix(p);
}

/*
 * 解析二元运算表达式（优先级爬升法）。
 *
 * limit 是当前允许的最低优先级：
 *   先解析左侧（一元/基本表达式），
 *   然后看下一个 token 是不是优先级 > limit 的二元运算符，
 *   是就吃进，递归解析右侧，组合成新的左子树，继续循环。
 *
 * 右结合运算符（.. ^）的递归调用用 prio-1，保证同优先级的右侧不被吃进。
 */
static Exp *parse_binop(Parser *p, int limit) {
    Exp *left = parse_unop(p);
    for (;;) {
        TokenType op = lex_current(&p->lex).type;
        int prio = binop_priority(op);
        if (prio == 0 || prio <= limit) break;
        int line = lex_current(&p->lex).line;
        lex_advance(&p->lex);
        int next_limit = is_right_assoc(op) ? prio - 1 : prio;
        Exp *right = parse_binop(p, next_limit);
        left = new_exp_binop(op, left, right, line);
    }
    return left;
}

/* 表达式入口 */
static Exp *parse_exp(Parser *p) {
    return parse_binop(p, 0);
}


/* ─── 语句解析 ─── */

/*
 * 解析语句块。
 *
 * block := { stat } [retstat]
 *
 * 一直解析语句，直到遇到 end / else / elseif / EOF 为止。
 * 返回语句链表头。
 */
static Stmt *parse_block(Parser *p) {
    Stmt *head = NULL;
    Stmt **tail = &head;
    for (;;) {
        Token t = lex_current(&p->lex);
        /* 块结束标志 */
        if (t.type == TK_END || t.type == TK_ELSE || t.type == TK_ELSEIF ||
            t.type == TK_EOF)
            break;

        Stmt *s = NULL;
        int line = t.line;

        switch (t.type) {
            case TK_SEMICOLON:
                /* 空语句，跳过 */
                lex_advance(&p->lex);
                continue;

            case TK_LOCAL: {
                /*
                 * local name = exp
                 * 或
                 * local function name(params) body end
                 *   （local function 允许递归引用自身，先定义名字再绑定函数体）
                 */
                lex_advance(&p->lex);
                if (lex_current(&p->lex).type == TK_FUNCTION) {
                    /* local function name(params) body end */
                    lex_advance(&p->lex);
                    char *name = expect_name(p);
                    expect(p, TK_LPAREN, "'('");
                    char **params = NULL;
                    int nparams = 0, cap = 0;
                    if (lex_current(&p->lex).type != TK_RPAREN) {
                        do {
                            char *pname = expect_name(p);
                            if (nparams >= cap) { cap = cap ? cap * 2 : 4; params = (char **)realloc(params, cap * sizeof(char *)); }
                            params[nparams++] = pname;
                        } while (accept(p, TK_COMMA));
                    }
                    expect(p, TK_RPAREN, "')'");
                    Stmt *body = parse_block(p);
                    expect(p, TK_END, "'end'");
                    s = new_stmt(STMT_FUNCTION, line);
                    s->u.func.name = name;
                    s->u.func.params = params;
                    s->u.func.nparams = nparams;
                    s->u.func.body = body;
                    break;
                }
                /* local name = exp */
                char *name = expect_name(p);
                Exp *value = NULL;
                if (accept(p, TK_ASSIGN))
                    value = parse_exp(p);
                else
                    value = new_exp_nil(line);
                s = new_stmt(STMT_LOCAL, line);
                s->u.local.name = name;
                s->u.local.value = value;
                break;
            }

            case TK_IF: {
                /*
                 * if exp then block { elseif exp then block } [ else block ] end
                 *
                 * elseif 链等价于嵌套 if：
                 *   if a then B1 elseif b then B2 else B3 end
                 *   ≡ if a then B1 else (if b then B2 else B3 end) end
                 */
                lex_advance(&p->lex);
                Exp *cond = parse_exp(p);
                expect(p, TK_THEN, "'then'");
                Stmt *then_branch = parse_block(p);
                Stmt *else_branch = NULL;
                Stmt **else_tail = &else_branch;
                /* elseif 链：每个 elseif 构造一个嵌套 if 挂到 else_tail */
                while (lex_current(&p->lex).type == TK_ELSEIF) {
                    int elseif_line = lex_current(&p->lex).line;
                    lex_advance(&p->lex);
                    Exp *ei_cond = parse_exp(p);
                    expect(p, TK_THEN, "'then'");
                    Stmt *ei_then = parse_block(p);
                    Stmt *nested = new_stmt(STMT_IF, elseif_line);
                    nested->u.if_.cond = ei_cond;
                    nested->u.if_.then_branch = ei_then;
                    nested->u.if_.else_branch = NULL;
                    *else_tail = nested;
                    else_tail = &nested->u.if_.else_branch;
                }
                /* else */
                if (accept(p, TK_ELSE))
                    *else_tail = parse_block(p);
                expect(p, TK_END, "'end'");
                s = new_stmt(STMT_IF, line);
                s->u.if_.cond = cond;
                s->u.if_.then_branch = then_branch;
                s->u.if_.else_branch = else_branch;
                break;
            }

            case TK_WHILE: {
                /* while exp do block end */
                lex_advance(&p->lex);
                Exp *cond = parse_exp(p);
                expect(p, TK_DO, "'do'");
                Stmt *body = parse_block(p);
                expect(p, TK_END, "'end'");
                s = new_stmt(STMT_WHILE, line);
                s->u.while_.cond = cond;
                s->u.while_.body = body;
                break;
            }

            case TK_FOR: {
                /* for var = start, end[, step] do block end */
                lex_advance(&p->lex);
                char *var = expect_name(p);
                expect(p, TK_ASSIGN, "'='");
                Exp *start = parse_exp(p);
                expect(p, TK_COMMA, "','");
                Exp *end = parse_exp(p);
                Exp *step = NULL;
                if (accept(p, TK_COMMA))
                    step = parse_exp(p);
                expect(p, TK_DO, "'do'");
                Stmt *body = parse_block(p);
                expect(p, TK_END, "'end'");
                s = new_stmt(STMT_FOR, line);
                s->u.for_.var = var;
                s->u.for_.start = start;
                s->u.for_.end = end;
                s->u.for_.step = step;
                s->u.for_.body = body;
                break;
            }

            case TK_FUNCTION: {
                /* function name ( params ) block end */
                lex_advance(&p->lex);
                char *name = expect_name(p);
                expect(p, TK_LPAREN, "'('");
                char **params = NULL;
                int nparams = 0, cap = 0;
                if (lex_current(&p->lex).type != TK_RPAREN) {
                    do {
                        char *pname = expect_name(p);
                        if (nparams >= cap) { cap = cap ? cap * 2 : 4; params = (char **)realloc(params, cap * sizeof(char *)); }
                        params[nparams++] = pname;
                    } while (accept(p, TK_COMMA));
                }
                expect(p, TK_RPAREN, "')'");
                Stmt *body = parse_block(p);
                expect(p, TK_END, "'end'");
                s = new_stmt(STMT_FUNCTION, line);
                s->u.func.name = name;
                s->u.func.params = params;
                s->u.func.nparams = nparams;
                s->u.func.body = body;
                break;
            }

            case TK_RETURN: {
                /* return [exp] */
                lex_advance(&p->lex);
                Exp *value = NULL;
                if (lex_current(&p->lex).type != TK_END &&
                    lex_current(&p->lex).type != TK_EOF &&
                    lex_current(&p->lex).type != TK_ELSE &&
                    lex_current(&p->lex).type != TK_ELSEIF &&
                    lex_current(&p->lex).type != TK_SEMICOLON)
                    value = parse_exp(p);
                s = new_stmt(STMT_RETURN, line);
                s->u.return_value = value;
                break;
            }

            case TK_BREAK: {
                lex_advance(&p->lex);
                s = new_stmt(STMT_BREAK, line);
                break;
            }

            case TK_LPAREN:
            case TK_LBRACE: {
                /*
                 * 以 ( 或 { 开头的语句：表达式语句（通常是函数调用）。
                 * 例：(f)(x) 或 ({1,2})[1]
                 * 也可能是索引赋值：t[k] = v（但 t 以 ( 或 { 开头）
                 */
                Exp *e = parse_exp(p);
                if (accept(p, TK_ASSIGN)) {
                    Exp *value = parse_exp(p);
                    if (e->type == EXP_INDEX) {
                        s = new_stmt(STMT_INDEX_ASSIGN, line);
                        s->u.index_assign.obj = e->u.index.obj;
                        s->u.index_assign.key = e->u.index.key;
                        s->u.index_assign.value = value;
                    } else {
                        fprintf(stderr, "语法错误（第 %d 行）：赋值目标必须是变量或索引\n", line);
                        exit(1);
                    }
                } else {
                    s = new_stmt(STMT_EXPR, line);
                    s->u.expr = e;
                }
                break;
            }

            case TK_NAME: {
                /*
                 * 以标识符开头的语句有三种：
                 *   1. 赋值：name = exp
                 *   2. 索引赋值：expr[key] = exp 或 expr.key = exp
                 *   3. 表达式语句：通常是函数调用 f(args)
                 *
                 * 解析一个表达式后，看后面是不是 '='：
                 *   是 → 赋值（表达式必须是变量引用或索引访问）
                 *   否 → 表达式语句
                 */
                Exp *e = parse_exp(p);
                if (accept(p, TK_ASSIGN)) {
                    Exp *value = parse_exp(p);
                    if (e->type == EXP_VAR) {
                        /* name = value */
                        s = new_stmt(STMT_ASSIGN, line);
                        s->u.assign.name = e->u.var;
                        s->u.assign.value = value;
                    } else if (e->type == EXP_INDEX) {
                        /* obj[key] = value */
                        s = new_stmt(STMT_INDEX_ASSIGN, line);
                        s->u.index_assign.obj = e->u.index.obj;
                        s->u.index_assign.key = e->u.index.key;
                        s->u.index_assign.value = value;
                    } else {
                        fprintf(stderr, "语法错误（第 %d 行）：赋值目标必须是变量或索引\n", line);
                        exit(1);
                    }
                } else {
                    s = new_stmt(STMT_EXPR, line);
                    s->u.expr = e;
                }
                break;
            }

            default:
                fprintf(stderr, "语法错误（第 %d 行）：意外的符号 %s\n",
                        line, token_to_string(t.type));
                exit(1);
        }

        /* 追加到语句链表 */
        *tail = s;
        tail = &s->next;
    }
    return head;
}

/* ─── 公开 API ─── */

Stmt *parse(const char *source) {
    Parser p;
    lex_init(&p.lex, source);
    Stmt *program = parse_block(&p);
    /* 期望读完所有 token */
    if (lex_current(&p.lex).type != TK_EOF) {
        Token t = lex_current(&p.lex);
        fprintf(stderr, "语法错误（第 %d 行）：意外的符号 %s（期望文件结束）\n",
                t.line, token_to_string(t.type));
        exit(1);
    }
    lex_free(&p.lex);
    return program;
}
