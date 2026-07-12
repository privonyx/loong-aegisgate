// TASK-20260528-01 Epic 2 — savingsStorytelling pure function tests
// TASK-20260614-03 — i18n 重构：pickAnalogy 仅返回 { unit, count }（labels 迁入
//   marketing.json）；SR2 反向校验迁移为遍历 marketing.json analogy.* 两语言文案。
//
// 覆盖（spec §6.1）：
//   1. < $0.10 → unit 'none'（防浮夸 / SR2）
//   2. $0.10 ≤ x < $50 → 'coffee'
//   3. $50 ≤ x < $500 → 'lunch'
//   4. x ≥ $500 → 'subscription'
//   5. count = 0 边界（$0.05 → none）
//   6. SR2 reverse: marketing analogy 文案（zh+en）不含 K2 banned terms

import { describe, it, expect } from 'vitest';
import { pickAnalogy } from '../lib/savingsStorytelling';
import zhMarketing from '../locales/zh-CN/marketing.json';
import enMarketing from '../locales/en-US/marketing.json';

describe('pickAnalogy (TASK-20260528-01 SR2)', () => {
  it('< $0.10 → unit "none"（防浮夸）', () => {
    expect(pickAnalogy(0.05).unit).toBe('none');
    expect(pickAnalogy(0.099).unit).toBe('none');
    expect(pickAnalogy(0).unit).toBe('none');
  });

  it('$0.10 ≤ x < $50 → coffee', () => {
    const r = pickAnalogy(5);
    expect(r.unit).toBe('coffee');
    expect(r.count).toBe(1);
  });

  it('$50 ≤ x < $500 → lunch', () => {
    const r = pickAnalogy(75);
    expect(r.unit).toBe('lunch');
    expect(r.count).toBe(5);
  });

  it('x ≥ $500 → subscription', () => {
    const r = pickAnalogy(1000);
    expect(r.unit).toBe('subscription');
    expect(r.count).toBe(50);
  });

  it('NaN / Infinity / 负值兜底为 unit "none"', () => {
    expect(pickAnalogy(NaN).unit).toBe('none');
    expect(pickAnalogy(Infinity).unit).toBe('none');
    expect(pickAnalogy(-10).unit).toBe('none');
  });

  it('SR2 reverse: marketing analogy 文案（zh+en）不含 K2 banned terms', () => {
    // K2 wording compliance: certified / production-grade / enterprise-ready /
    // guaranteed must NEVER appear in marketing analogy copy (defense in depth)。
    const bannedTerms = ['certified', 'production-grade', 'enterprise-ready', 'guaranteed'];
    const collect = (a: typeof zhMarketing['analogy']): string[] => [
      a.savedVsBaseline, a.baseline, a.equivalent,
      a.unit.coffee, a.unit.lunch, a.unit.subscription,
    ];
    const allCopy = [...collect(zhMarketing.analogy), ...collect(enMarketing.analogy)];
    for (const copy of allCopy) {
      for (const banned of bannedTerms) {
        expect(copy.toLowerCase()).not.toContain(banned);
      }
    }
  });
});
