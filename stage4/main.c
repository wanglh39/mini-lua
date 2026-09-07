/*
 * main.c - 解释器入口（命令行参数解析、脚本加载、REPL）
 * 官方对照: lua-5.1.5/src/main.c
 * 实现阶段: 阶段 2
 *
 * 用法：
 *   mini-lua script.lua      执行脚本文件
 *   mini-lua                 进入 REPL（交互式）
 *   mini-lua -e "code"       执行命令行代码
 *
 * 阶段 2 流程：源码 → 词法 → 语法(AST) → 编译(字节码) → VM 执行
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lparser.h"
#include "lcode.h"
#include "lvm.h"
#include "lauxlib.h"

/* 读取整个文件到内存 */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "无法打开文件: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/*
 * 执行一段 Lua 源码。
 *
 * 流程：源码 → parse(AST) → compile(Proto) → VM 执行
 */
static int run_source(const char *source) {
    Stmt *program = parse(source);
    Proto *main_proto = compile_main(program);
    lua_State *L = state_create();
    luaL_openlibs(L->globals);
    execute_main(L, main_proto);
    state_free(L);
    return 0;
}

/*
 * REPL — 交互式解释器。
 *
 * 阶段 2 简化：逐行读取，每行作为独立 chunk 解析+编译+执行。
 */
static int repl(void) {
    char line[1024];
    printf("mini-lua 0.2 (阶段 2: 栈式字节码 VM)\n");
    printf("输入 Ctrl+Z (Windows) 或 Ctrl+D (Unix) 退出\n\n");

    lua_State *L = state_create();
    luaL_openlibs(L->globals);

    for (;;) {
        printf("> ");
        if (!fgets(line, sizeof(line), stdin))
            break;
        if (line[0] == '\n' || line[0] == '\0')
            continue;
        Stmt *program = parse(line);
        Proto *main_proto = compile_main(program);
        execute_main(L, main_proto);
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "-e") == 0)
        return run_source(argv[2]);
    if (argc >= 2) {
        char *source = read_file(argv[1]);
        if (!source) return 1;
        int result = run_source(source);
        free(source);
        return result;
    }
    return repl();
}
