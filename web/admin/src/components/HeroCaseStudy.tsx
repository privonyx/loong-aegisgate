// TASK-20260528-01 — HeroCaseStudy: Row 0 hero 组件（spec §4.2.3）
//
// 整合三大卖点（spec §1.2 三大核心诉求）：
//   1. SavingsAnalogy — counting-up 数字 + 多档自适应类比单位
//   2. CacheInterceptStat — 已为你拦截 N 次 + 三类柱
//   3. QualityTrendBadge — 既有 EMA + slope（迁移自 27-02 Row 4）
//
// 空数据兜底（spec §1.3 设计原则 #1）：data=null 显示引导文案 + CTA 链 ADOPTERS.md
// SR4 字面量 #1 "Case Study Numbers" hard-coded 在 h2 / 4 方共享

import { Gauge, Rocket, ArrowRight } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import SavingsAnalogy from './SavingsAnalogy';
import CacheInterceptStat from './CacheInterceptStat';
import type { CaseStudyHeadline } from '../types';

interface Props {
  data: CaseStudyHeadline | null;
}

export default function HeroCaseStudy({ data }: Props) {
  const { t } = useTranslation('marketing');

  if (!data) {
    return (
      <section className="rounded-2xl border border-border bg-gradient-to-br from-card to-bg p-8 sm:p-10">
        <header className="flex items-center gap-3 mb-4">
          <Rocket size={20} className="text-primary" />
          <h2 className="text-xl sm:text-2xl font-semibold tracking-tight">{t('hero.title')}</h2>
        </header>
        <p className="text-sm text-muted max-w-2xl mb-4">
          {t('hero.empty.desc')}
        </p>
        <div className="flex flex-wrap gap-3 text-sm">
          <a
            href="/docs/quickstart"
            className="inline-flex items-center gap-1 text-primary hover:underline"
          >
            {t('hero.empty.quickstart')} <ArrowRight size={14} />
          </a>
          <span className="text-muted">·</span>
          <a
            href="https://github.com/privonyx/loong-aegisgate/blob/main/ADOPTERS.md"
            className="inline-flex items-center gap-1 text-primary hover:underline"
            target="_blank"
            rel="noopener noreferrer"
          >
            {t('hero.empty.joinAdopters')}
          </a>
        </div>
      </section>
    );
  }

  const sinceLabel = data.aggregator_since
    ? t('hero.since.date', { date: data.aggregator_since.slice(0, 10) })
    : t('hero.since.launch');
  const scopeLabel = data.scope === 'global'
    ? t('hero.scope.global')
    : t('hero.scope.tenant', { id: data.tenant_id ?? '' });
  const q = data.quality_reason;
  const qualityAccent = q.slope >= 0 ? 'text-success' : 'text-warning';

  return (
    <section className="rounded-2xl border border-border bg-gradient-to-br from-card via-card to-bg p-6 sm:p-8 space-y-5">
      {/* Header */}
      <header className="flex flex-wrap items-baseline justify-between gap-2">
        <div className="flex items-center gap-3">
          <Rocket size={22} className="text-primary" />
          <h2 className="text-xl sm:text-2xl font-semibold tracking-tight">{t('hero.title')}</h2>
        </div>
        <p className="text-xs text-muted tabular-nums">
          <span className="text-fg/70">{scopeLabel}</span>
          {' · '}
          <span>{sinceLabel}</span>
        </p>
      </header>

      {/* 3 column hero grid */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <SavingsAnalogy
          costSaved={data.saved_vs_baseline.cost_saved}
          savingsPercent={data.saved_vs_baseline.savings_percent}
          baselineCost={data.saved_vs_baseline.baseline_cost}
        />
        <CacheInterceptStat data={data.cache_hit_by_type} />

        {/* Quality trend (迁移自 27-02 Row 4 第 3 张卡 / 保留功能) */}
        <div className="flex flex-col gap-2 p-6 rounded-xl bg-gradient-to-br from-fg/5 to-fg/0 border border-border">
          <div className={`flex items-center gap-2 ${qualityAccent}`}>
            <Gauge size={16} className="shrink-0" />
            <span className="text-xs uppercase tracking-wider">{t('quality.label')}</span>
          </div>
          <div className="flex items-baseline gap-2">
            <span className={`text-4xl sm:text-5xl font-bold tracking-tight tabular-nums ${qualityAccent}`}>
              {q.current_ema.toFixed(3)}
            </span>
            <span className={`text-sm ${qualityAccent}`}>
              {q.slope >= 0 ? '↑' : '↓'} {Math.abs(q.slope).toFixed(3)}
            </span>
          </div>
          <p className="text-xs text-muted">
            {t('quality.factuality')} <span className="text-fg font-medium">{q.reason_factuality}</span>
            {' · '}{t('quality.refusal')} <span className="text-fg font-medium">{q.reason_refusal}</span>
            {' · '}{t('quality.latency')} <span className="text-fg font-medium">{q.reason_latency_degraded}</span>
          </p>
        </div>
      </div>

      {/* CTA line（A2：随 locale 切换） */}
      <p className="text-xs text-muted border-t border-border pt-3">
        {t('hero.cta.question')}
        {' '}
        <a href="/docs/quickstart" className="text-primary hover:underline">
          {t('hero.cta.quickstart')}
        </a>
        {' / '}
        <a
          href="https://github.com/privonyx/loong-aegisgate/blob/main/ADOPTERS.md"
          className="text-primary hover:underline"
          target="_blank"
          rel="noopener noreferrer"
        >
          ADOPTERS.md
        </a>
        {` / ${t('hero.cta.runPrefix')} `}
        <code className="px-1 py-0.5 rounded bg-fg/5 text-fg/80">aegisctl estimate</code>
        {' '}{t('hero.cta.estimateSuffix')}
      </p>
    </section>
  );
}
