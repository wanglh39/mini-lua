/*
 * lparser.h - 语法分析器（FuncState、expdesc、单遍编译上下文）
 * 官方对照: lua-5.1.5/src/lparser.h
 * 实现阶段: 阶段 1
 *
 * 阶段 1 说明：
 *   官方 Lua 的 lparser 是单遍编译——边解析边生成字节码，不建 AST。
 *   阶段 1 是树遍历解释器，需要先建 AST 再执行，所以这里改成"递归下降建 AST"。
 *   阶段 2 会重写为官方的单遍编译风格。
 *
 * 用法：
 *   Stmt *program = parse(source);   // 返回语句链表头（chunk）
 *   ... 执行 program ...
 *   （阶段 1 不释放 AST，程序退出时 OS 回收）
 */

#ifndef lparser_h
#define lparser_h

#include "llex.h"
#include "lobject.h"

/*
 * 语法分析器状态。
 *
 * 持有一个 Lexer，通过 lex_current/lex_advance 读 token。
 * 阶段 1 不需要 FuncState（那是单遍编译的状态），所以 Parser 很简单。
 */
typedef struct Parser {
    Lexer lex;
} Parser;

/*
 * parse — 解析源码，返回 AST 根（语句链表头）。
 *
 * 入口：chunk := block
 * 出错时打印错误信息并 exit(1)（阶段 1 简化处理）。
 */
Stmt *parse(const char *source);

#endif /* lparser_h */
