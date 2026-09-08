/*
 * llex.h - 词法分析器（LexState、Token 类型、保留字表）
 * 官方对照: lua-5.1.5/src/llex.h
 * 实现阶段: 阶段 1
 *
 * 词法分析器把源码字符流切分成 Token 序列。
 * 语法分析器通过 lex_current() 查看当前 Token，lex_advance() 消费并读取下一个。
 */

#ifndef llex_h
#define llex_h

/*
 * Token 类型枚举。
 *
 * 分四组：
 *   1. 字面量（数字、字符串、标识符）
 *   2. 保留字（local / if / while / function …）
 *   3. 运算符与标点（+ - * / == ~= ( ) ; , …）
 *   4. 特殊（EOF）
 *
 * 保留字单独列出来，语法分析器可以直接 switch 判断，比"先当标识符再查表"更直观。
 */
typedef enum {
    /* 字面量 */
    TK_NUMBER,      /* 3.14, 42 */
    TK_STRING,      /* "hello", 'world' */
    TK_NAME,        /* 标识符：变量名、函数名 */

    /* 保留字 */
    TK_LOCAL,       /* local */
    TK_IF,          /* if */
    TK_THEN,        /* then */
    TK_ELSE,        /* else */
    TK_ELSEIF,      /* elseif */
    TK_END,         /* end */
    TK_WHILE,       /* while */
    TK_DO,          /* do */
    TK_FOR,         /* for */
    TK_IN,          /* in */
    TK_FUNCTION,    /* function */
    TK_RETURN,      /* return */
    TK_TRUE,        /* true */
    TK_FALSE,       /* false */
    TK_NIL,         /* nil */
    TK_AND,         /* and */
    TK_OR,          /* or */
    TK_NOT,         /* not */
    TK_BREAK,       /* break */

    /* 运算符 */
    TK_PLUS,        /* +  */
    TK_MINUS,       /* -  */
    TK_STAR,        /* *  */
    TK_SLASH,       /* /  */
    TK_PERCENT,     /* %  */
    TK_CARET,       /* ^  */
    TK_EQ,          /* == */
    TK_NE,          /* ~= */
    TK_LT,          /* <  */
    TK_GT,          /* >  */
    TK_LE,          /* <= */
    TK_GE,          /* >= */
    TK_ASSIGN,      /* =  （赋值，不是 ==） */
    TK_CONCAT,      /* .. */
    TK_LEN,         /* #  */

    /* 标点 */
    TK_LPAREN,      /* (  */
    TK_RPAREN,      /* )  */
    TK_LBRACE,      /* {  */
    TK_RBRACE,      /* }  */
    TK_LBRACKET,    /* [  */
    TK_RBRACKET,    /* ]  */
    TK_SEMICOLON,   /* ;  */
    TK_COMMA,       /* ,  */
    TK_DOT,         /* .  */

    /* 特殊 */
    TK_EOF,         /* 源码结束 */
} TokenType;

/*
 * Token 结构。
 *
 * 对于 TK_NUMBER：用 value.n（double）
 * 对于 TK_STRING / TK_NAME：用 value.s（动态分配的 C 字符串）
 * 其余类型不使用 value。
 *
 * line 记录 token 出现的行号，语法/运行时错误信息要用。
 */
typedef struct Token {
    TokenType type;
    int line;
    union {
        double n;
        char *s;
    } value;
} Token;

/*
 * 词法分析器状态。
 *
 * source  — 源码 C 字符串（以 '\0' 结尾）
 * pos     — 当前读取位置（下标）
 * line    — 当前行号（从 1 开始）
 * current — 当前 token（lookahead，已预读）
 *
 * 用法：
 *   Lexer lex;
 *   lex_init(&lex, source);
 *   while (lex_current(&lex).type != TK_EOF) {
 *       Token t = lex_current(&lex);
 *       ... 处理 t ...
 *       lex_advance(&lex);
 *   }
 *   lex_free(&lex);
 */
typedef struct Lexer {
    const char *source;
    int pos;
    int line;
    Token current;
} Lexer;

/* 初始化词法分析器，预读第一个 token */
void lex_init(Lexer *lex, const char *source);

/* 释放词法分析器持有的内存（current token 中的字符串） */
void lex_free(Lexer *lex);

/* 返回当前 token（不消费） */
Token lex_current(Lexer *lex);

/* 消费当前 token，读取下一个 */
void lex_advance(Lexer *lex);

/* 把 token 类型转成可读字符串（错误信息用） */
const char *token_to_string(TokenType type);

#endif /* llex_h */
