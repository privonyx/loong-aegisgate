// TASK-20260614-03 — i18n 初始化（react-i18next 单例）。
//
// 副作用 import（main.tsx / test/setup.ts 顶部 import 即完成初始化）。
// useTranslation 依赖此处 initReactI18next 注册的全局实例，无需每处包 Provider。
//
// 安全：localStorage 仅接受 SUPPORTED_LANGS 白名单值；非法值回退默认语言。

import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import { resources } from './resources';

export const SUPPORTED_LANGS = ['zh-CN', 'en-US'] as const;
export type Lang = (typeof SUPPORTED_LANGS)[number];
export const DEFAULT_LANG: Lang = 'zh-CN';
export const STORAGE_KEY = 'aegis-locale';

export function isLang(value: unknown): value is Lang {
  return value === 'zh-CN' || value === 'en-US';
}

// 持久化优先 → navigator 语言检测（非 zh* 视为 en-US）→ 默认。
export function detectLang(): Lang {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (isLang(saved)) return saved;
  } catch {
    // localStorage 不可用（隐私模式等）时静默回退
  }
  const nav =
    typeof navigator !== 'undefined' && navigator.language
      ? navigator.language.toLowerCase()
      : '';
  return nav.startsWith('zh') ? 'zh-CN' : 'en-US';
}

i18n.use(initReactI18next).init({
  resources,
  lng: detectLang(),
  fallbackLng: DEFAULT_LANG,
  defaultNS: 'common',
  ns: Object.keys(resources['zh-CN']),
  interpolation: { escapeValue: false },
  returnNull: false,
});

export default i18n;
