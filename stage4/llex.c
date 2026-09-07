/*
 * llex.c - 词法分析实现（逐 token 识别、数字/字符串/注释处理）
 * 官方对照: lua-5.1.5/src/llex.c
 * 实现阶段: 阶段 1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "llex.h"

/* ─── 内部辅助 ─── */

/* 保留字查表：读到一个标识符后，判断它是不是保留字 */
static TokenType check_keyword(const char *word) {
    /* 按字母排序便于对照；数量少，线性查找即可 */
    struct { const char *name; TokenType type; } table[] = {
        {"and", TK_AND},       {"break", TK_BREAK},   {"do", TK_DO},
        {"else", TK_ELSE},     {"elseif", TK_ELSEIF}, {"end", TK_END},
        {"false", TK_FALSE},   {"for", TK_FOR},       {"function", TK_FUNCTION},
        {"if", TK_IF},         {"in", TK_IN},         {"local", TK_LOCAL},
        {"nil", TK_NIL},       {"not", TK_NOT},       {"or", TK_OR},
        {"return", TK_RETURN}, {"then", TK_THEN},     {"true", TK_TRUE},
        {"while", TK_WHILE},
    };
    int n = sizeof(table) / sizeof(table[0]);
    for (int i = 0; i < n; i++) {
        if (strcmp(word, table[i].name) == 0)
            return table[i].type;
    }
    return TK_NAME;  /* 不是保留字，就是普通标识符 */
}

/* 跳过空白字符和注释，把 lex->pos 停在下一个有效字符 */
static void skip_whitespace(Lexer *lex) {
    for (;;) {
        char c = lex->source[lex->pos];
        if (c == '\0') return;
        if (c == '\n') { lex->line++; lex->pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { lex->pos++; continue; }
        /* 注释：-- 单行，--[[ 多行（阶段 1 简化：只支持单行） */
        if (c == '-' && lex->source[lex->pos + 1] == '-') {
            lex->pos += 2;
            /* 多行注释 --[[ ... ]] */
            if (lex->source[lex->pos] == '[' && lex->source[lex->pos + 1] == '[') {
                lex->pos += 2;
                while (lex->source[lex->pos] != '\0') {
                    if (lex->source[lex->pos] == ']' &&
                        lex->source[lex->pos + 1] == ']') {
                        lex->pos += 2;
                        return;
                    }
                    if (lex->source[lex->pos] == '\n') lex->line++;
                    lex->pos++;
                }
                return;  /* 源码结束（注释未闭合，简化处理） */
            }
            /* 单行注释：跳到行尾 */
            while (lex->source[lex->pos] != '\0' && lex->source[lex->pos] != '\n')
                lex->pos++;
            continue;
        }
        return;  /* 不是空白也不是注释，停下 */
    }
}

/* 读取标识符或保留字 */
static Token read_identifier(Lexer *lex) {
    int start = lex->pos;
    int line = lex->line;
    /* 标识符：字母/下划线开头，后跟字母/下划线/数字 */
    while (isalnum((unsigned char)lex->source[lex->pos]) ||
           lex->source[lex->pos] == '_')
        lex->pos++;
    int len = lex->pos - start;
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, lex->source + start, len);
    buf[len] = '\0';
    TokenType type = check_keyword(buf);
    Token t = {type, line, {0}};
    if (type == TK_NAME)
        t.value.s = buf;   /* 标识符需要保存名字 */
    else
        free(buf);         /* 保留字不需要名字 */
    return t;
}

/*
 * 读取数字字面量。
 * 阶段 1 简化：直接用 strtod 解析，支持整数、小数、科学记数法、负号（负号由一元运算处理）。
 * 不支持 0x 十六进制（官方 5.1 支持，阶段 1 可省略）。
 */
static Token read_number(Lexer *lex) {
    int line = lex->line;
    int start = lex->pos;
    /* 数字：[0-9]+ . [0-9]* [eE] [+-]? [0-9]+ */
    while (isdigit((unsigned char)lex->source[lex->pos])) lex->pos++;
    if (lex->source[lex->pos] == '.') {
        lex->pos++;
        while (isdigit((unsigned char)lex->source[lex->pos])) lex->pos++;
    }
    if (lex->source[lex->pos] == 'e' || lex->source[lex->pos] == 'E') {
        lex->pos++;
        if (lex->source[lex->pos] == '+' || lex->source[lex->pos] == '-')
            lex->pos++;
        while (isdigit((unsigned char)lex->source[lex->pos])) lex->pos++;
    }
    /* 复制子串，用 strtod 解析 */
    int len = lex->pos - start;
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, lex->source + start, len);
    buf[len] = '\0';
    Token t = {TK_NUMBER, line, {0}};
    t.value.n = strtod(buf, NULL);
    free(buf);
    return t;
}

/*
 * 读取字符串字面量（引号开头）。
 * 支持 "..." 和 '...'，支持常见转义：\n \t \r \\ \" \' \0
 */
static Token read_string(Lexer *lex) {
    int line = lex->line;
    char quote = lex->source[lex->pos];  /* ' 或 " */
    lex->pos++;
    /* 动态数组收集字符 */
    int cap = 16, len = 0;
    char *buf = (char *)malloc(cap);
    while (lex->source[lex->pos] != quote && lex->source[lex->pos] != '\0') {
        char c = lex->source[lex->pos];
        if (c == '\n') lex->line++;  /* 多行字符串（简化允许） */
        if (c == '\\') {
            /* 转义序列 */
            lex->pos++;
            char esc = lex->source[lex->pos];
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"';  break;
                case '\'': c = '\''; break;
                case '0': c = '\0'; break;
                default: c = esc; break;  /* 未知转义：原样保留 */
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        buf[len++] = c;
        lex->pos++;
    }
    buf[len] = '\0';
    if (lex->source[lex->pos] == quote) lex->pos++;  /* 跳过闭合引号 */
    Token t = {TK_STRING, line, {0}};
    t.value.s = buf;
    return t;
}

/*
 * 读取一个完整 token（内部函数）。
 * 调用前已跳过空白和注释。
 */
static Token read_token(Lexer *lex) {
    char c = lex->source[lex->pos];
    int line = lex->line;

    if (c == '\0') return (Token){TK_EOF, line, {0}};

    /* 标识符 / 保留字 */
    if (isalpha((unsigned char)c) || c == '_') return read_identifier(lex);

    /* 数字 */
    if (isdigit((unsigned char)c)) return read_number(lex);

    /* 字符串 */
    if (c == '"' || c == '\'') return read_string(lex);

    /* 运算符与标点（需要向前看一两个字符来消歧） */
    lex->pos++;
    switch (c) {
        case '+': return (Token){TK_PLUS, line, {0}};
        case '-': return (Token){TK_MINUS, line, {0}};
        case '*': return (Token){TK_STAR, line, {0}};
        case '/': return (Token){TK_SLASH, line, {0}};
        case '%': return (Token){TK_PERCENT, line, {0}};
        case '^': return (Token){TK_CARET, line, {0}};
        case '#': return (Token){TK_LEN, line, {0}};
        case '(': return (Token){TK_LPAREN, line, {0}};
        case ')': return (Token){TK_RPAREN, line, {0}};
        case '{': return (Token){TK_LBRACE, line, {0}};
        case '}': return (Token){TK_RBRACE, line, {0}};
        case '[': return (Token){TK_LBRACKET, line, {0}};
        case ']': return (Token){TK_RBRACKET, line, {0}};
        case ';': return (Token){TK_SEMICOLON, line, {0}};
        case ',': return (Token){TK_COMMA, line, {0}};
        case '.':
            /* .. 是连接运算符，. 是成员访问（阶段 1 暂不支持成员访问） */
            if (lex->source[lex->pos] == '.') {
                lex->pos++;
                return (Token){TK_CONCAT, line, {0}};
            }
            return (Token){TK_DOT, line, {0}};
        case '=':
            if (lex->source[lex->pos] == '=') {
                lex->pos++;
                return (Token){TK_EQ, line, {0}};
            }
            return (Token){TK_ASSIGN, line, {0}};
        case '~':
            if (lex->source[lex->pos] == '=') {
                lex->pos++;
                return (Token){TK_NE, line, {0}};
            }
            break;  /* ~ 后面不是 =，词法错误 */
        case '<':
            if (lex->source[lex->pos] == '=') {
                lex->pos++;
                return (Token){TK_LE, line, {0}};
            }
            return (Token){TK_LT, line, {0}};
        case '>':
            if (lex->source[lex->pos] == '=') {
                lex->pos++;
                return (Token){TK_GE, line, {0}};
            }
            return (Token){TK_GT, line, {0}};
        default:
            break;
    }

    /* 词法错误 */
    fprintf(stderr, "词法错误（第 %d 行）：无法识别的字符 '%c'\n", line, c);
    exit(1);
}

/* 释放 token 中动态分配的字符串 */
static void free_token(Token *t) {
    if (t->type == TK_NAME || t->type == TK_STRING)
        free(t->value.s);
}

/* ─── 公开 API ─── */

void lex_init(Lexer *lex, const char *source) {
    lex->source = source;
    lex->pos = 0;
    lex->line = 1;
    skip_whitespace(lex);
    lex->current = read_token(lex);
}

void lex_free(Lexer *lex) {
    free_token(&lex->current);
}

Token lex_current(Lexer *lex) {
    return lex->current;
}

void lex_advance(Lexer *lex) {
    free_token(&lex->current);
    skip_whitespace(lex);
    lex->current = read_token(lex);
}

const char *token_to_string(TokenType type) {
    switch (type) {
        case TK_NUMBER:   return "number";
        case TK_STRING:   return "string";
        case TK_NAME:     return "name";
        case TK_LOCAL:    return "local";
        case TK_IF:       return "if";
        case TK_THEN:     return "then";
        case TK_ELSE:     return "else";
        case TK_ELSEIF:   return "elseif";
        case TK_END:      return "end";
        case TK_WHILE:    return "while";
        case TK_DO:       return "do";
        case TK_FOR:      return "for";
        case TK_IN:       return "in";
        case TK_FUNCTION: return "function";
        case TK_RETURN:   return "return";
        case TK_TRUE:     return "true";
        case TK_FALSE:    return "false";
        case TK_NIL:      return "nil";
        case TK_AND:      return "and";
        case TK_OR:       return "or";
        case TK_NOT:      return "not";
        case TK_BREAK:    return "break";
        case TK_PLUS:     return "+";
        case TK_MINUS:    return "-";
        case TK_STAR:     return "*";
        case TK_SLASH:    return "/";
        case TK_PERCENT:  return "%";
        case TK_CARET:    return "^";
        case TK_EQ:       return "==";
        case TK_NE:       return "~=";
        case TK_LT:       return "<";
        case TK_GT:       return ">";
        case TK_LE:       return "<=";
        case TK_GE:       return ">=";
        case TK_ASSIGN:   return "=";
        case TK_CONCAT:   return "..";
        case TK_LEN:      return "#";
        case TK_LPAREN:   return "(";
        case TK_RPAREN:   return ")";
        case TK_LBRACE:   return "{";
        case TK_RBRACE:   return "}";
        case TK_LBRACKET: return "[";
        case TK_RBRACKET: return "]";
        case TK_SEMICOLON:return ";";
        case TK_COMMA:    return ",";
        case TK_DOT:      return ".";
        case TK_EOF:      return "<EOF>";
        default:          return "?";
    }
}
