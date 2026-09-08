import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  title: 'mini-lua',
  description: '从零用 C 实现 Lua 5.1 子集——学习底层原理与设计哲学',

  base: '/mini-lua/',
  ignoreDeadLinks: true,

  themeConfig: {
    siteTitle: 'mini-lua',

    nav: [
      { text: '首页', link: '/' },
      { text: '设计哲学', link: '/philosophy/' },
      { text: '实现笔记', link: '/chapters/' },
      { text: '对照官方', link: '/vs-official/' },
      { text: '性能对比', link: '/benchmarks/' },
    ],

    sidebar: {
      '/philosophy/': [
        {
          text: '设计哲学',
          items: [
            { text: '为什么只有 table', link: '/philosophy/why-only-table' },
            { text: '为什么寄存器 VM', link: '/philosophy/why-register-vm' },
            { text: '为什么 1-based 索引', link: '/philosophy/why-1-based' },
            { text: '增量 GC 与写屏障', link: '/philosophy/incremental-gc' },
            { text: '非对称协程', link: '/philosophy/why-asymmetric-coroutine' },
          ],
        },
      ],
      '/chapters/': [
        {
          text: '实现笔记',
          items: [
            { text: '阶段 1：树遍历解释器', link: '/chapters/01-treewalk' },
            { text: '阶段 2：栈式字节码 VM', link: '/chapters/02-stack-vm' },
            { text: '阶段 3：闭包 + table + metatable', link: '/chapters/03-closure-table' },
            { text: '阶段 4：寄存器式 VM', link: '/chapters/04-register-vm' },
            { text: '阶段 5a：增量 GC', link: '/chapters/05a-gc' },
            { text: '阶段 5b：coroutine', link: '/chapters/05b-coroutine' },
          ],
        },
      ],
      '/vs-official/': [
        {
          text: '对照官方源码',
          items: [
            { text: '总览', link: '/vs-official/' },
          ],
        },
      ],
      '/benchmarks/': [
        {
          text: '性能对比',
          items: [
            { text: '总览', link: '/benchmarks/' },
          ],
        },
      ],
    },

    socialLinks: [
      { icon: 'github', link: 'https://github.com/wanglh39/mini-lua' },
    ],

    footer: {
      message: 'MIT Licensed',
      copyright: 'Copyright © 2026 mini-lua contributors',
    },

    outline: {
      level: 2,
      label: '本页目录',
    },

    docFooter: {
      prev: '上一页',
      next: '下一页',
    },

    lastUpdated: {
      text: '最后更新于',
    },
  },
})