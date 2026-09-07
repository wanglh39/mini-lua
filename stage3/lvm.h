/*
 * lvm.h - VM 内部接口（luaV_execute 主循环、luaV_gettable）
 * 官方对照: lua-5.1.5/src/lvm.h
 * 实现阶段: 阶段 2+
 *
 * 阶段 2/3：栈式字节码 VM。
 *
 * 架构：
 *   值栈（stack）— 表达式计算的临时值
 *   调用帧栈（frames）— 每层函数调用一个帧，存局部变量和指令指针
 *   全局环境（globals）— 全局变量（Env 链表，阶段 3 改为 table）
 *
 * 执行流程：
 *   call_function(fn, args) → 创建调用帧 → 绑定参数 → while(ip < code_size) switch(op) → 返回
 */

#ifndef lvm_h
#define lvm_h

#include "lobject.h"

/* ─── 全局环境（阶段 2 仍用链表，阶段 3 改为 table） ─── */

typedef struct Binding {
    char *name;
    lua_Value value;
    struct Binding *next;
} Binding;

struct Env {
    Binding *bindings;
    Env *parent;
};

Env *env_create(Env *parent);
void env_define(Env *env, const char *name, lua_Value value);
lua_Value *env_lookup(Env *env, const char *name);
void env_assign(Env *env, const char *name, lua_Value value);

/* ─── 调用帧 ─── */

/*
 * 每次函数调用创建一个帧。
 *
 * fn     — 被调用的闭包（含 Proto + upvalue）
 * locals — 局部变量数组（槽位 0..nlocals-1）
 * ip     — 指令指针（当前执行到哪条指令）
 */
typedef struct CallFrame {
    Function *fn;
    lua_Value *locals;
    int ip;
} CallFrame;

/* ─── VM 状态 ─── */

/*
 * lua_State — 整个 VM 的运行时状态。
 *
 * stack      — 值栈（表达式计算的临时值）
 * top        — 栈顶下标（下一个可写位置）
 * frames     — 调用帧栈
 * frame_count — 当前帧数
 * globals    — 全局变量环境
 * open_upvals — open upvalue 链表（所有未关闭的 UpVal，按栈位置降序）
 *
 * Upvalue 引用化（阶段 3 关键改进）：
 *   open UpVal 的 ptr 指向某帧的 locals 槽位。
 *   函数返回时遍历 open_upvals，把指向该帧的 UpVal close 掉。
 */
typedef struct lua_State {
    lua_Value *stack;
    int top;
    int stack_cap;
    CallFrame *frames;
    int frame_count;
    int frame_cap;
    Env *globals;
    UpVal *open_upvals;  /* open upvalue 链表头 */
} lua_State;

/* 创建/销毁 lua_State */
lua_State *state_create(void);
void state_free(lua_State *L);

/*
 * 函数调用——VM 核心。
 *
 * 内置函数：直接调用 C 函数指针。
 * 用户函数：创建调用帧，绑定参数，执行字节码主循环，返回结果。
 */
lua_Value call_function(lua_State *L, Function *fn, lua_Value *args, int nargs);

/*
 * 执行主 chunk。
 *
 * 编译后的 Proto 包装成 Function（闭包），调用 call_function 执行。
 * globals 在 L->globals 里。
 */
lua_Value execute_main(lua_State *L, Proto *main_proto);

#endif /* lvm_h */
