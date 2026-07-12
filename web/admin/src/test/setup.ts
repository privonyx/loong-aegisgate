import '@testing-library/jest-dom/vitest';
// TASK-20260614-03 — 初始化 i18n 单例（同步资源），使所有组件测试中的
// useTranslation 拿到全局实例。默认语言由 detectLang 决定（happy-dom 下
// navigator.language 通常为 en-US，但测试断言中文文案时显式切到 zh-CN）。
import i18n from '../i18n';
import { beforeEach } from 'vitest';

// 测试默认锁定 zh-CN：现有中文断言据此保持绿色（迁移期最小改动）。
beforeEach(() => {
  if (i18n.language !== 'zh-CN') {
    i18n.changeLanguage('zh-CN');
  }
});

