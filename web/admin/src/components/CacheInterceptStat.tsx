// TASK-20260528-01 — CacheInterceptStat: 缓存命中叙事子组件（spec §4.2.4）
// TASK-20260614-03 — i18n（A2 纯净分离）：文案迁入 marketing namespace，随语言切换；
//   拦截叙事用 <Trans> 保留数字加粗强调。
//   - 三类横向柱（exact 蓝 / semantic 紫 / conversation 青）按比例

import { Zap } from 'lucide-react';
import { Trans, useTranslation } from 'react-i18next';
import { useCountUp } from '../hooks/useCountUp';
import type { CaseStudyCacheHitByType } from '../types';

interface Props {
  data: CaseStudyCacheHitByType;
}

export default function CacheInterceptStat({ data }: Props) {
  const { t } = useTranslation('marketing');
  const intercepted = data.hit_exact + data.hit_semantic + data.hit_conversation;
  const total = intercepted + data.miss;
  const animatedIntercept = useCountUp({ to: intercepted, durationMs: 1000, decimals: 0 });
  const hitRatePct = data.total_hit_rate * 100;

  // 三色柱宽度按 intercepted 内部占比（避免 miss 占主体显得太弱）。
  const interceptedDenom = Math.max(1, intercepted);
  const exactPct = (data.hit_exact / interceptedDenom) * 100;
  const semanticPct = (data.hit_semantic / interceptedDenom) * 100;
  const convPct = (data.hit_conversation / interceptedDenom) * 100;

  return (
    <div className="flex flex-col gap-3 p-6 rounded-xl bg-gradient-to-br from-accent/10 to-accent/5 border border-accent/20">
      <div className="flex items-center gap-2 text-accent/80">
        <Zap size={16} className="shrink-0" />
        <span className="text-xs uppercase tracking-wider">{t('cache.label')}</span>
      </div>
      <div className="flex items-baseline gap-2">
        <span className="text-4xl sm:text-5xl font-bold text-accent tracking-tight tabular-nums">
          {hitRatePct.toFixed(1)}%
        </span>
        <span className="text-sm text-accent/80">{t('cache.hitRate')}</span>
      </div>
      <p className="text-sm text-fg">
        <Trans
          t={t}
          i18nKey="cache.intercepted"
          values={{ count: animatedIntercept }}
          components={{ b: <span className="font-semibold text-accent tabular-nums" /> }}
        />
        <span className="text-muted">
          {t('cache.total', { total, pct: hitRatePct.toFixed(1) })}
        </span>
      </p>
      {intercepted > 0 && (
        <div className="flex h-2 rounded-full overflow-hidden bg-card border border-border">
          <div
            className="bg-[hsl(210,100%,60%)]"
            style={{ width: `${exactPct}%` }}
            title={`${t('cache.type.exact')} ${data.hit_exact}`}
          />
          <div
            className="bg-[hsl(280,80%,60%)]"
            style={{ width: `${semanticPct}%` }}
            title={`${t('cache.type.semantic')} ${data.hit_semantic}`}
          />
          <div
            className="bg-[hsl(190,95%,50%)]"
            style={{ width: `${convPct}%` }}
            title={`${t('cache.type.conversation')} ${data.hit_conversation}`}
          />
        </div>
      )}
      <p className="text-xs text-muted">
        {t('cache.type.exact')} <span className="text-fg font-medium">{data.hit_exact}</span>
        {' · '}{t('cache.type.semantic')} <span className="text-fg font-medium">{data.hit_semantic}</span>
        {' · '}{t('cache.type.conversation')} <span className="text-fg font-medium">{data.hit_conversation}</span>
      </p>
    </div>
  );
}
