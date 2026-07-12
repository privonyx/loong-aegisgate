// TASK-20260614-03 — 测试渲染工具：包裹 I18nextProvider，默认 zh-CN。
//
// 现有测试可继续用 @testing-library/react 的 render（setup.ts 已全局初始化
// i18n 并锁 zh-CN）；当需要显式验证某语言渲染时用 renderWithI18n(ui, { lang }).

import type { ReactElement } from 'react';
import { render, type RenderOptions } from '@testing-library/react';
import { I18nextProvider } from 'react-i18next';
import i18n, { type Lang } from '../i18n';

interface I18nRenderOptions extends Omit<RenderOptions, 'wrapper'> {
  lang?: Lang;
}

export function renderWithI18n(
  ui: ReactElement,
  { lang = 'zh-CN', ...options }: I18nRenderOptions = {},
) {
  if (i18n.language !== lang) {
    i18n.changeLanguage(lang);
  }
  return render(<I18nextProvider i18n={i18n}>{ui}</I18nextProvider>, options);
}

export { i18n };
