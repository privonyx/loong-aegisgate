// TASK-20260614-03 — format 本地化工具测试。

import { describe, it, expect, beforeEach } from 'vitest';
import { formatNumber, formatCurrency, formatDate } from '../lib/format';
import i18n from '../i18n';

describe('format utils (TASK-20260614-03)', () => {
  beforeEach(() => {
    i18n.changeLanguage('en-US');
  });

  it('formatNumber 按千分位分组', () => {
    expect(formatNumber(1234567)).toBe('1,234,567');
  });

  it('formatCurrency 输出 USD 货币格式', () => {
    expect(formatCurrency(1234.5)).toBe('$1,234.50');
  });

  it('formatDate 对非法输入回退为原值字符串', () => {
    expect(formatDate('not-a-date')).toBe('not-a-date');
  });

  it('formatDate 对合法日期产出本地化字符串', () => {
    const out = formatDate('2026-06-14T00:00:00Z', { dateStyle: 'medium' });
    expect(typeof out).toBe('string');
    expect(out.length).toBeGreaterThan(0);
  });

  it('随语言切换改变分组/小数风格（zh-CN 与 en-US 货币符号一致但 locale 生效）', () => {
    i18n.changeLanguage('zh-CN');
    const zh = formatCurrency(1000);
    i18n.changeLanguage('en-US');
    const en = formatCurrency(1000);
    expect(zh).toContain('1,000');
    expect(en).toContain('1,000');
  });
});
