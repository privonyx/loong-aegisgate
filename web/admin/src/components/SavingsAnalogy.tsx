// TASK-20260528-01 — SavingsAnalogy: 类比单位展示子组件（spec §4.2.4）
// TASK-20260614-03 — i18n（A2 纯净分离）：文案迁入 marketing namespace，
//   随语言切换；类比行用 <Trans> 保留数字加粗强调。
//
// 用 useCountUp hook 做 cost_saved 跳数动画，pickAnalogy 做单位映射。
// count=0 (e.g. $0.05) 或 unit='none' 时不渲染类比行（防"0 杯咖啡"浮夸）。

import { TrendingDown } from 'lucide-react';
import { Trans, useTranslation } from 'react-i18next';
import { useCountUp } from '../hooks/useCountUp';
import { pickAnalogy } from '../lib/savingsStorytelling';

interface Props {
  costSaved: number;
  savingsPercent: number;
  baselineCost: number;
}

export default function SavingsAnalogy({ costSaved, savingsPercent, baselineCost }: Props) {
  const { t } = useTranslation('marketing');
  const animated = useCountUp({ to: costSaved, durationMs: 1200, decimals: 2 });
  const analogy = pickAnalogy(costSaved);
  const showAnalogy = analogy.unit !== 'none' && analogy.count > 0;

  return (
    <div className="flex flex-col gap-2 p-6 rounded-xl bg-gradient-to-br from-success/10 to-success/5 border border-success/20">
      <div className="flex items-center gap-2 text-success/80">
        <TrendingDown size={16} className="shrink-0" />
        <span className="text-xs uppercase tracking-wider">{t('analogy.savedVsBaseline')}</span>
      </div>
      <div className="flex items-baseline gap-2">
        <span className="text-4xl sm:text-5xl font-bold text-success tracking-tight tabular-nums">
          ${animated}
        </span>
        {savingsPercent > 0 && (
          <span className="text-sm text-success/80">
            ↓ {savingsPercent.toFixed(1)}% ({t('analogy.baseline', { cost: baselineCost.toFixed(2) })})
          </span>
        )}
      </div>
      {showAnalogy && (
        <p className="text-sm text-muted">
          <Trans
            t={t}
            i18nKey="analogy.equivalent"
            values={{ count: analogy.count, unit: t(`analogy.unit.${analogy.unit}`) }}
            components={{ b: <span className="font-semibold text-fg" /> }}
          />
        </p>
      )}
    </div>
  );
}
