/*
 * lvm.h - VM 内部接口（luaV_execute 主循环、luaV_gettable）
 * 官方对照: lua-5.1.5/src/lvm.h
 * 实现阶段: 阶段 4+
 *
 * 阶段 4：寄存器式字节码 VM。
 *
 * 架构：
 *   寄存器数组（regs）— 每个调用帧有自己的寄存器窗口 R[0..nregs-1]
 *     R[0..nparams-1]       — 函数参数
 *     R[nparams..nlocals-1] — 局部变量
 *     R[nlocals..nregs-1]   — 表达式求值的临时寄存器
 *   调用帧栈（frames）— 每层函数调用一个帧，存寄存器数组和指令指针
 *   全局环境（globals）— 全局变量（Env 链表）
 *
 * 对比阶段 2/3（栈式）：
 *   栈式 VM 有一个全局值栈，指令通过 push/pop 操作栈顶。
 *   寄存器式 VM 每个帧有独立寄存器数组，指令直接读写 R[A]/R[B]/R[C]。
 *   无需 push/pop，指令更少，访问更直接。
 *
 * 执行流程：
 *   call_function(fn, args) → 创建调用帧 → 绑定参数到 R[0..] →
 *   while(ip < code_size) switch(op) 读写寄存器 → 返回 R[A]
 */

#ifndef lvm_h
#define lvm_h

#include "lobject.h"

/* ─── 全局环境（链表实现，阶段 3 的 table 也可用，这里保留链表简化） ─── */

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
 * fn   — 被调用的闭包（含 Proto + upvalue）
 * regs — 寄存器数组（槽位 0..nregs-1），替代阶段 2/3 的 locals
 *         R[0..nparams-1] 是参数，R[nparams..nlocals-1] 是局部变量，
 *         R[nlocals..nregs-1] 是临时寄存器。
 * ip   — 指令指针（当前执行到哪条指令）
 */
typedef struct CallFrame {
    Function *fn;
    lua_Value *regs;
    int ip;
} CallFrame;

/* ─── VM 状态 ─── */

/*
 * lua_State — 整个 VM 的运行时状态。
 *
 * frames      — 调用帧栈
 * frame_count — 当前帧数
 * globals     — 全局变量环境
 * open_upvals — open upvalue 链表（所有未关闭的 UpVal，按寄存器地址降序）
 *
 * 注意：阶段 4 不再需要值栈（stack/top），
 *   每个帧有独立寄存器数组，指令直接读写 R[A]/R[B]/R[C]。
 *   内置函数调用时，参数直接从调用方的寄存器传递（&R[A+1]）。
 *
 * Upvalue 引用化（阶段 3 引入，阶段 4 保留）：
 *   open UpVal 的 ptr 指向某帧的 regs 槽位。
 *   函数返回时遍历 open_upvals，把指向该帧的 UpVal close 掉。
 */
typedef struct lua_State {
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
 * 用户函数：创建调用帧，绑定参数到 R[0..nparams-1]，执行字节码主循环，返回 R[A]。
 *
 * args 指向调用方的寄存器数组（&R[A+1]），nargs 是参数个数。
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
