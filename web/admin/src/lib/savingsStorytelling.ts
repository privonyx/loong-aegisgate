// TASK-20260528-01 — savingsStorytelling: 多档自适应类比单位映射（spec D4=C）
// TASK-20260614-03 — i18n 重构：pickAnalogy 只返回 { unit, count }（纯数据），
//   单位文案迁入 marketing.json 的 analogy.unit.*，由渲染层 t() 取词（A2 纯净分离）。
//
// 设计原则（SR2 K2 wording 合规 + 防浮夸）：
//   - 不浮夸：savedUsd < $0.10 时 unit='none'（不显示类比行）
//   - 不夸大：单位映射阈值（咖啡 $5 / 午餐 $15 / 月度订阅 $20）走"具体可感知"路线
//   - K2 wording 合规由 marketing.json 文案 + 迁移版 SR2 测试反向校验
//   - 0 新依赖：纯函数 / 0 副作用 / 易测
//
// 阈值（spec §4.2.2）：
//   savedUsd < 0.10       → none
//   0.10 ≤ savedUsd < 50  → coffee   ($5/unit)
//   50 ≤ savedUsd < 500   → lunch    ($15/unit)
//   savedUsd ≥ 500        → subscription ($20/unit, 代指月度 SaaS 订阅)

export type AnalogyUnit = 'coffee' | 'lunch' | 'subscription' | 'none';

export interface AnalogyResult {
  unit: AnalogyUnit;
  count: number;
}

const NONE: AnalogyResult = { unit: 'none', count: 0 };

export function pickAnalogy(savedUsd: number): AnalogyResult {
  if (!Number.isFinite(savedUsd) || savedUsd < 0.10) {
    return NONE;
  }
  if (savedUsd < 50) {
    return { unit: 'coffee', count: Math.floor(savedUsd / 5) };
  }
  if (savedUsd < 500) {
    return { unit: 'lunch', count: Math.floor(savedUsd / 15) };
  }
  return { unit: 'subscription', count: Math.floor(savedUsd / 20) };
}
