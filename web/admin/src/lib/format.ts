// TASK-20260614-03 — locale 感知的日期/数字/货币格式化。
//
// 统一替换裸 toLocaleString()，按 i18n.language 选 Intl locale。

import i18n from '../i18n';

function currentLocale(): string {
  return i18n.language || 'zh-CN';
}

export function formatNumber(
  value: number,
  options?: Intl.NumberFormatOptions,
): string {
  if (!Number.isFinite(value)) return String(value);
  return new Intl.NumberFormat(currentLocale(), options).format(value);
}

export function formatCurrency(value: number, currency = 'USD'): string {
  if (!Number.isFinite(value)) return String(value);
  return new Intl.NumberFormat(currentLocale(), {
    style: 'currency',
    currency,
  }).format(value);
}

export function formatDate(
  value: string | number | Date,
  options?: Intl.DateTimeFormatOptions,
): string {
  const d = value instanceof Date ? value : new Date(value);
  if (Number.isNaN(d.getTime())) return String(value);
  return new Intl.DateTimeFormat(
    currentLocale(),
    options ?? { dateStyle: 'medium', timeStyle: 'short' },
  ).format(d);
}
