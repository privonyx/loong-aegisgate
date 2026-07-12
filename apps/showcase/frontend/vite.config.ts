/// <reference types="vitest/config" />
import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// Vite 配置在 Node 下执行；此处仅做最小环境声明，避免引入完整 @types/node。
declare const process: { env: Record<string, string | undefined> };
const BACKEND = process.env.SHOWCASE_BACKEND_URL ?? 'http://localhost:8090';
// timeout/proxyTimeout 放宽到 200s，避免长耗时生成（多集大纲等）被代理层提前掐断。
const proxy = { '/api': { target: BACKEND, changeOrigin: true, timeout: 200_000, proxyTimeout: 200_000 } };

export default defineConfig({
  plugins: [react()],
  server: { port: 5173, proxy },
  preview: { port: 5173, host: true, proxy },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
  },
});
