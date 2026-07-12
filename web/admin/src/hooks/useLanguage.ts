// TASK-20260614-03 — useLanguage：当前语言 + 切换 + localStorage 持久化。
//
// 同构借鉴 useTheme.ts 的 localStorage 范式。语言切换通过 i18next
// changeLanguage 触发全局重渲染。

import { useTranslation } from 'react-i18next';
import { isLang, STORAGE_KEY, SUPPORTED_LANGS, type Lang } from '../i18n';

export function useLanguage() {
  const { i18n } = useTranslation();
  const lang: Lang = isLang(i18n.language) ? i18n.language : 'zh-CN';

  const setLang = (next: Lang) => {
    if (!isLang(next)) return;
    i18n.changeLanguage(next);
    try {
      localStorage.setItem(STORAGE_KEY, next);
    } catch {
      // localStorage 不可用时仍切换内存语言
    }
    if (typeof document !== 'undefined') {
      document.documentElement.lang = next;
    }
  };

  return { lang, setLang, languages: SUPPORTED_LANGS };
}
