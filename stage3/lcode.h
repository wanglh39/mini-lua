/*
 * lcode.h - 编码器接口（寄存器分配、指令发射、expdesc 表达式分类）
 * 官方对照: lua-5.1.5/src/lcode.h
 * 实现阶段: 阶段 2+
 *
 * 阶段 2 编译器：把 AST 编译成栈式字节码（Proto）。
 *
 * 编译流程：AST → compile_exp/compile_stat → 指令序列 → Proto
 *   表达式编译约定：生成的指令执行后，结果在值栈顶。
 *   语句编译约定：生成的指令执行后，值栈恢复原状（无残留）。
 */

#ifndef lcode_h
#define lcode_h

#include "lobject.h"

/* 编译主 chunk（顶层语句块）为 Proto */
Proto *compile_main(Stmt *ast);

#endif /* lcode_h */
