// TASK-20260614-03 — useLanguage hook 测试。

import { describe, it, expect, beforeEach } from 'vitest';
import { renderHook, act } from '@testing-library/react';
import { useLanguage } from '../hooks/useLanguage';
import i18n, { STORAGE_KEY } from '../i18n';

describe('useLanguage (TASK-20260614-03)', () => {
  beforeEach(() => {
    localStorage.clear();
    i18n.changeLanguage('zh-CN');
  });

  it('返回当前语言', () => {
    const { result } = renderHook(() => useLanguage());
    expect(result.current.lang).toBe('zh-CN');
  });

  it('setLang 切换语言并写入 localStorage', () => {
    const { result } = renderHook(() => useLanguage());
    act(() => result.current.setLang('en-US'));
    expect(i18n.language).toBe('en-US');
    expect(localStorage.getItem(STORAGE_KEY)).toBe('en-US');
  });

  it('setLang 忽略非法语言值', () => {
    const { result } = renderHook(() => useLanguage());
    act(() => {
      // @ts-expect-error 故意传非法值验证防御
      result.current.setLang('fr-FR');
    });
    expect(i18n.language).toBe('zh-CN');
    expect(localStorage.getItem(STORAGE_KEY)).toBeNull();
  });

  it('暴露受支持语言列表', () => {
    const { result } = renderHook(() => useLanguage());
    expect(result.current.languages).toEqual(['zh-CN', 'en-US']);
  });
});
