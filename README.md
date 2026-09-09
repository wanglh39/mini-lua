# mini-lua

> 从零用 C 实现的 Lua 5.1 子集，用于学习 Lua 的底层原理与设计哲学。

本项目以 [Lua 5.1.5](https://www.lua.org/versions.html#5.1) 官方源码为对照基准，分 5 个阶段递进实现，最终覆盖：单遍编译、寄存器式字节码 VM、闭包 upvalue、table、metatable、增量标记-清除 GC、非对称协程。

## 为什么做这个

Lua 的设计哲学本身就是教科书——优先级 **简单性 > 效率 > 完整性**，每个特性都经过深思熟虑的取舍。学 Lua 不只是学"怎么实现一门语言"，更是学"如何做减法"。

官方 Lua 5.1 核心约 17,500 行 C 代码，体量在"踮脚够得着"的甜点区：不像 Lisp 太简单学不到字节码/GC/寄存器分配，不像 Python/JS 太复杂完不成。

## 路线图

详见 [ROADMAP.md](./ROADMAP.md)。

| 阶段 | 目标 | 对照官方文件 | 状态 |
|------|------|-------------|------|
| 1 | 树遍历解释器（Lua 极小子集） | — | 未开始 |
| 2 | 栈式字节码 VM | `lopcodes.h` `lvm.c` `lcode.c` | 未开始 |
| 3 | 闭包 + table + metatable | `ltable.c` `lfunc.c` `ltm.c` | 未开始 |
| 4 | 寄存器式 VM | `lcode.c` + Lua 5.0 论文 | 未开始 |
| 5a | 增量标记-清除 GC | `lgc.c` `lstring.c` | 未开始 |
| 5b | coroutine | `lstate.c` `ldo.c` | 未开始 |

## 构建

依赖：gcc、make。

```bash
make            # 编译生成 mini-lua
make run        # 编译并进入 REPL
make test       # 运行 tests/*.lua
make bench      # 运行 benchmarks/*.lua
make clean      # 清理构建产物
```

## 目录结构

```
mini-lua/
├── src/                 # 源码（文件名沿用官方 Lua 5.1 命名，便于对照）
│   ├── lua.h            # 公开 API
│   ├── lualib.h         # 标准库注册
│   ├── lauxlib.h        # C 辅助库
│   ├── lobject.h/c      # TValue / GCObject 对象系统
│   ├── lstate.h/c       # lua_State / CallInfo
│   ├── lopcodes.h       # 字节码指令集
│   ├── lvm.h/c          # 虚拟机主循环
│   ├── lcode.h/c        # 编码器（寄存器分配）
│   ├── lparser.h/c      # 语法分析（单遍编译）
│   ├── llex.h/c         # 词法分析
│   ├── ltable.h/c       # table（数组+哈希合一）
│   ├── lfunc.h/c        # 闭包 / upvalue
│   ├── lgc.h/c          # 增量 GC
│   ├── ltm.h/c          # 元方法表
│   ├── ldo.h/c          # 调用栈管理
│   ├── lmem.h/c         # 内存管理
│   ├── lstring.h/c      # 字符串池（短/长串分离）
│   ├── lapi.h/c         # C API 内部实现
│   ├── ldebug.h/c       # 调试接口
│   ├── lundump.h/c      # 字节码反序列化
│   ├── lzio.h/c         # 输入流抽象
│   ├── lbaselib.c       # 基础库
│   ├── lmathlib.c       # 数学库
│   ├── lstrlib.c        # 字符串库
│   ├── ltablib.c        # table 库
│   ├── linit.c          # 库注册入口
│   └── main.c           # 解释器入口
├── docs/                # VitePress 文档站
│   ├── philosophy/      # 设计哲学解读
│   ├── chapters/        # 逐阶段实现笔记
│   ├── vs-official/     # 与官方源码对照差异
│   └── benchmarks/      # 性能对比
├── tests/               # 测试用例（.lua 脚本）
├── benchmarks/          # 性能基准
└── .github/workflows/   # CI / 文档站部署
```

## 参考资料与对照源码

| 资料 | 说明 |
|------|------|
| [Lua 5.1.5 官方源码](https://www.lua.org/ftp/lua-5.1.5.tar.gz) | 第一手对照基准，注释极其详尽 |
| 《Lua 设计与实现》codedump 著 | 中文，基于 5.1，逐文件讲解 |
| 《自己动手实现 Lua》张秀宏 著 | C++ 实现的 5.1，有完整可跑代码 |
| [The Implementation of Lua 5.0 (JOS 2005)](https://www.lua.org/doc/josl05.pdf) | 阶段 4 核心论文，栈式→寄存器式转换 |

## 文档站

**在线访问**：<https://wanglh39.github.io/mini-lua/>

```bash
cd docs && npm install && npm run dev    # 本地预览
```

部署到 GitHub Pages：推送 main 分支即自动构建（见 `.github/workflows/deploy-docs.yml`）。

## 许可证

MIT