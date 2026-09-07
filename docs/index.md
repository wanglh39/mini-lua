---
layout: home

hero:
  name: mini-lua
  text: 从零实现 Lua 5.1 子集
  tagline: 用 C 逐阶段实现一门真实语言，学习底层原理与设计哲学
  actions:
    - theme: brand
      text: 开始阅读
      link: /chapters/
    - theme: alt
      text: 设计哲学
      link: /philosophy/
    - theme: alt
      text: GitHub
      link: https://github.com/your-username/mini-lua

features:
  - title: 设计哲学即教材
    details: Lua 的优先级——简单性 > 效率 > 完整性。每个特性都是深思熟虑的取舍，学 Lua 是学"如何做减法"。
  - title: 5 阶段递进
    details: 树遍历 → 栈式 VM → 闭包/table → 寄存器 VM → GC/协程。每阶段有可演示产出，不会闷头写三个月。
  - title: 对照官方源码
    details: 以 Lua 5.1.5 为答案书，文件名一一对应（ltable.c、lgc.c…），卡住时直接对照看。
  - title: 覆盖现代语言实现核心技术
    details: 单遍编译、寄存器分配、upvalue 闭包、增量三色 GC、非对称协程——一门项目全打通。
---