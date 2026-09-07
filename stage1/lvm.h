/*
 * lvm.h - 树遍历解释器接口（eval_exp / exec_stmt / call_function）
 * 官方对照: lua-5.1.5/src/lvm.h
 * 实现阶段: 阶段 1（树遍历解释器）
 *
 * 阶段 1 与阶段 2+ 的关键区别：
 *   - 无字节码、无 VM 主循环、无值栈、无调用帧
 *   - 直接递归遍历 AST 执行：eval_exp 求值表达式，exec_stmt 执行语句
 *   - 变量存储在 Env 链表中（非 locals 数组）
 *   - 闭包 = 函数体 + 定义时 Env（非 Proto + UpVal）
 *
 * 执行流程：
 *   parse(source) → AST → eval_program(env, ast) → 遍历执行
 */

#ifndef lvm_h
#define lvm_h

#include "lobject.h"

/*
 * Ctrl — 控制流状态。
 *
 * 树遍历解释器中，return 和 break 需要中断正常执行流程。
 * exec_stmt 返回 Ctrl，调用方检查 control 字段决定是否继续。
 *
 * CONTROL_NORMAL：正常执行，继续下一条语句
 * CONTROL_RETURN：遇到 return，retval 是返回值，停止执行剩余语句
 * CONTROL_BREAK：遇到 break，停止执行循环体
 */
typedef enum {
    CONTROL_NORMAL,
    CONTROL_RETURN,
    CONTROL_BREAK,
} ControlType;

typedef struct {
    ControlType control;
    lua_Value retval;  /* CONTROL_RETURN 时的返回值 */
} Ctrl;

/*
 * eval_exp — 求值表达式，返回 lua_Value。
 *
 * 递归遍历 AST 表达式节点：
 *   EXP_NUMBER → 直接返回数字值
 *   EXP_VAR → 从 env 中查找变量
 *   EXP_BINOP → 递归求值左右操作数，执行运算
 *   EXP_CALL → 递归求值函数和参数，调用 call_function
 *   EXP_FUNCTION → 创建闭包（捕获当前 env）
 */
lua_Value eval_exp(Env *env, Exp *e);

/*
 * exec_stmt — 执行语句，返回控制流状态。
 *
 * 递归遍历 AST 语句节点：
 *   STMT_EXPR → 求值表达式，丢弃结果
 *   STMT_LOCAL → 求值表达式，在 env 中定义新绑定
 *   STMT_IF → 求值条件，按真值执行 then/else 分支
 *   STMT_WHILE → 循环求值条件并执行体
 *   STMT_RETURN → 返回 CONTROL_RETURN + 返回值
 *   STMT_BREAK → 返回 CONTROL_BREAK
 */
Ctrl exec_stmt(Env *env, Stmt *s);

/*
 * exec_block — 执行语句链表（语句块）。
 *
 * 逐条执行 stmt 链表，遇到 return/break 时中断并传播控制流。
 */
Ctrl exec_block(Env *env, Stmt *block);

/*
 * call_function — 调用函数。
 *
 * 内置函数：直接调用 C 函数指针。
 * 用户函数：创建新 env（parent = 闭包捕获的 env），绑定参数，执行 body。
 *   如果 body 返回 CONTROL_RETURN，取 retval；否则返回 nil。
 */
lua_Value call_function(Function *fn, lua_Value *args, int nargs);

/*
 * eval_program — 执行主 chunk（全局环境 + 语句链表）。
 *
 * main.c 调用：parse → eval_program
 */
void eval_program(Env *globals, Stmt *program);

#endif /* lvm_h */