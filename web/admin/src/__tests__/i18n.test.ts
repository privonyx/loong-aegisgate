// TASK-20260614-03 — i18n 基础设施测试（阶段 0 脚手架）
//
// 覆盖：
//   1. isLang 语言白名单校验（安全：localStorage 仅接受枚举值）
//   2. detectLang 持久化优先 → navigator 检测回退
//   3. i18n 初始化：zh-CN fallback + common 默认 namespace
//   4. common namespace 两语言均有译文

import { describe, it, expect, beforeEach } from 'vitest';
import i18n, {
  detectLang,
  isLang,
  DEFAULT_LANG,
  SUPPORTED_LANGS,
  STORAGE_KEY,
} from '../i18n';

describe('i18n infrastructure (TASK-20260614-03)', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('isLang 校验语言白名单（防注入非法 locale）', () => {
    expect(isLang('zh-CN')).toBe(true);
    expect(isLang('en-US')).toBe(true);
    expect(isLang('fr-FR')).toBe(false);
    expect(isLang('')).toBe(false);
    expect(isLang(null)).toBe(false);
    expect(isLang(undefined)).toBe(false);
  });

  it('detectLang 优先返回 localStorage 中的合法 locale', () => {
    localStorage.setItem(STORAGE_KEY, 'en-US');
    expect(detectLang()).toBe('en-US');
    localStorage.setItem(STORAGE_KEY, 'zh-CN');
    expect(detectLang()).toBe('zh-CN');
  });

  it('detectLang 忽略非法持久化值，回退到受支持语言', () => {
    localStorage.setItem(STORAGE_KEY, 'xx-YY');
    expect(SUPPORTED_LANGS).toContain(detectLang());
  });

  it('i18n 以 zh-CN 为 fallback 初始化', () => {
    expect(DEFAULT_LANG).toBe('zh-CN');
    expect(i18n.options.fallbackLng).toContain('zh-CN');
    expect(i18n.options.defaultNS).toBe('common');
  });

  it('common namespace 两语言均有 actions 译文', () => {
    expect(i18n.getResource('zh-CN', 'common', 'actions.export')).toBeTruthy();
    expect(i18n.getResource('en-US', 'common', 'actions.export')).toBeTruthy();
  });
});
