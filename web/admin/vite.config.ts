/// <reference types="vitest/config" />
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// SPA 部署在 /admin/ 子路径下：
// - base: '/admin/' 让 Vite build 输出的资源引用 /admin/assets/...
// - BrowserRouter basename="/admin" 让客户端路由匹配 /admin/<page>
// - 后端 main.cpp 用单 regex handler 同时服务 /admin/* 静态资源 + SPA fallback
export default defineConfig({
  plugins: [react(), tailwindcss()],
  base: '/admin/',
  server: {
    proxy: {
      '/admin/api': 'http://localhost:8080',
      '/admin/auth': 'http://localhost:8080',
      '/admin/ws': { target: 'ws://localhost:8080', ws: true },
    }
  },
  build: {
    outDir: 'dist',
    // TASK-20260602-01 Epic 8 — recharts 单独 chunk。recharts (~2.15) 是当前
    // bundle 最大单一依赖，懒加载页 (Dashboard/Savings/Costs) 各自 import
    // 一次会被 Vite 重复打入各 lazy chunk。手动 manualChunks 将其抽到独立 vendor
    // 文件，被首次 charts 页加载，后续页直接命中浏览器缓存。
    rollupOptions: {
      output: {
        manualChunks: {
          recharts: ['recharts'],
        },
      },
    },
  },
  test: {
    globals: true,
    // 使用 happy-dom 替代 jsdom：jsdom@29 通过 html-encoding-sniffer 间接 require
    // 一个 ESM-only 的 @exodus/bytes/encoding-lite.js，在 Node ESM 严格模式下抛
    // ERR_REQUIRE_ESM，导致所有 React 组件级测试无法启动。happy-dom 不依赖该链路，
    // 同时升级到 ^20.9.0 修复 CVE GHSA-96g7-g7g9-jxw8（VM Context Escape RCE）。
    environment: 'happy-dom',
    setupFiles: ['./src/test/setup.ts'],
    css: false,
  }
})
