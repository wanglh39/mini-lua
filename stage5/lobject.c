/*
 * lobject.c - 对象基本操作辅助（luaO_int2fb 整数压缩编码等）
 * 官方对照: lua-5.1.5/src/lobject.c
 * 实现阶段: 阶段 2+
 *
 * 阶段 1 内容：值构造辅助函数 + 值输出 + 真值判断 + 相等判断。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobject.h"

lua_Value make_nil(void) {
    lua_Value v;
    v.type = LUA_TNIL;
    return v;
}

lua_Value make_boolean(int b) {
    lua_Value v;
    v.type = LUA_TBOOLEAN;
    v.v.b = b ? 1 : 0;
    return v;
}

lua_Value make_number(double n) {
    lua_Value v;
    v.type = LUA_TNUMBER;
    v.v.n = n;
    return v;
}

lua_Value make_string(const char *s) {
    lua_Value v;
    v.type = LUA_TSTRING;
    v.v.s = strdup(s);  /* 复制一份，调用方可以放心释放原串 */
    return v;
}

lua_Value make_string_owned(char *s) {
    lua_Value v;
    v.type = LUA_TSTRING;
    v.v.s = s;  /* 直接接管，不再复制 */
    return v;
}

lua_Value make_function(Function *fn) {
    lua_Value v;
    v.type = LUA_TFUNCTION;
    v.v.fn = fn;
    return v;
}

lua_Value make_table(Table *t) {
    lua_Value v;
    v.type = LUA_TTABLE;
    v.v.t = t;
    return v;
}

lua_Value make_thread(lua_Thread *th) {
    lua_Value v;
    v.type = LUA_TTHREAD;
    v.v.th = th;
    return v;
}

const char *type_name(LuaType type) {
    switch (type) {
        case LUA_TNIL:      return "nil";
        case LUA_TBOOLEAN:  return "boolean";
        case LUA_TNUMBER:   return "number";
        case LUA_TSTRING:   return "string";
        case LUA_TFUNCTION: return "function";
        case LUA_TTABLE:    return "table";
        case LUA_TTHREAD:   return "thread";
        default:            return "unknown";
    }
}

/*
 * print_value — 值的文本输出。
 *
 * 数字：整数就输出整数形式（3 而非 3.0），小数输出小数。
 *   Lua 5.1 的数字是 double，但显示时整数不带小数点。
 * 字符串：直接输出内容（不加引号，print 的语义）。
 * 函数：输出 function 0x地址（调试用）。
 */
void print_value(lua_Value v) {
    switch (v.type) {
        case LUA_TNIL:
            printf("nil");
            break;
        case LUA_TBOOLEAN:
            printf(v.v.b ? "true" : "false");
            break;
        case LUA_TNUMBER:
            /* 整数显示为整数 */
            if (v.v.n == (double)(long)v.v.n)
                printf("%ld", (long)v.v.n);
            else
                printf("%g", v.v.n);
            break;
        case LUA_TSTRING:
            printf("%s", v.v.s);
            break;
        case LUA_TFUNCTION:
            printf("function: 0x%p", (void *)v.v.fn);
            break;
        case LUA_TTABLE:
            printf("table: 0x%p", (void *)v.v.t);
            break;
        case LUA_TTHREAD:
            printf("thread: 0x%p", (void *)v.v.th);
            break;
    }
}

/*
 * is_truthy — Lua 的真值判断。
 *
 * Lua 中只有 nil 和 false 为假，其余（包括 0、空串）都为真。
 * 这和 C/Python 不同（C 的 0 为假，Python 的空串为假）。
 */
int is_truthy(lua_Value v) {
    if (v.type == LUA_TNIL) return 0;
    if (v.type == LUA_TBOOLEAN && v.v.b == 0) return 0;
    return 1;
}

/*
 * value_equals — 值的相等判断（== 运算）。
 *
 * 类型不同直接不等。
 * 数字按浮点比较，字符串按 strcmp，布尔按值，函数按指针。
 * nil == nil 为真。
 */
int value_equals(lua_Value a, lua_Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case LUA_TNIL:      return 1;
        case LUA_TBOOLEAN:  return a.v.b == b.v.b;
        case LUA_TNUMBER:   return a.v.n == b.v.n;
        case LUA_TSTRING:   return strcmp(a.v.s, b.v.s) == 0;
        case LUA_TFUNCTION: return a.v.fn == b.v.fn;
        case LUA_TTABLE:    return a.v.t == b.v.t;
        case LUA_TTHREAD:   return a.v.th == b.v.th;
        default:            return 0;
    }
}

/* op_name — 指令名转字符串（调试/反汇编用） */
const char *op_name(OpCode op) {
    switch (op) {
        case OP_LOADK:      return "LOADK";
        case OP_LOADNIL:    return "LOADNIL";
        case OP_LOADBOOL:   return "LOADBOOL";
        case OP_MOVE:       return "MOVE";
        case OP_GETUPVAL:   return "GETUPVAL";
        case OP_SETUPVAL:   return "SETUPVAL";
        case OP_GETGLOBAL:  return "GETGLOBAL";
        case OP_SETGLOBAL:  return "SETGLOBAL";
        case OP_NEWTABLE:   return "NEWTABLE";
        case OP_GETTABLE:   return "GETTABLE";
        case OP_SETTABLE:   return "SETTABLE";
        case OP_SETTABLEK:  return "SETTABLEK";
        case OP_ADD:        return "ADD";
        case OP_SUB:        return "SUB";
        case OP_MUL:        return "MUL";
        case OP_DIV:        return "DIV";
        case OP_MOD:        return "MOD";
        case OP_POW:        return "POW";
        case OP_EQ:         return "EQ";
        case OP_NE:         return "NE";
        case OP_LT:         return "LT";
        case OP_GT:         return "GT";
        case OP_LE:         return "LE";
        case OP_GE:         return "GE";
        case OP_NEG:        return "NEG";
        case OP_NOT:        return "NOT";
        case OP_LEN:        return "LEN";
        case OP_CONCAT:     return "CONCAT";
        case OP_JMP:        return "JMP";
        case OP_TEST:       return "TEST";
        case OP_TESTN:      return "TESTN";
        case OP_CALL:       return "CALL";
        case OP_RETURN:     return "RETURN";
        case OP_CLOSURE:    return "CLOSURE";
        default:            return "?";
    }
}
