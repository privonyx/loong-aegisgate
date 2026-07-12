import { useState } from 'react';
import type { ObservabilitySummary, ValueEvent } from '../types';

interface Props {
  summary: ObservabilitySummary | null;
  recentEvents: ValueEvent[];
}

function fmt(n: number): string {
  return n.toLocaleString('zh-CN');
}

export function ValuePanel({ summary, recentEvents }: Props) {
  const [open, setOpen] = useState(false);

  const tokensSaved = summary?.totalTokensSaved ?? 0;
  const cacheHitRate = summary ? Math.round(summary.cacheHitRate * 100) : 0;
  const creationReqs = summary?.byEngine.creation.requests ?? 0;
  const guardrailBlocks = summary?.guardrailBlocks ?? 0;
  const complianceReqs = summary?.byEngine.compliance.requests ?? 0;
  const avgLatency = summary?.avgLatencyMs ?? 0;
  // 估算节省金额：按每 1K token ≈ ￥0.01 的保守口径（仅演示，可在 README 说明）
  const savedYuan = (tokensSaved / 1000) * 0.01;

  return (
    <aside className="value-panel" aria-label="价值面板">
      <h2 className="panel-title">价值面板 · 双核心</h2>

      <div className="dual-cards">
        <section className="big-card card-saving" aria-label="创作引擎省钱">
          <div className="card-eyebrow">创作引擎 · 省钱</div>
          <div className="card-metric" data-testid="tokens-saved">
            {fmt(tokensSaved)}
            <span className="card-unit">tokens 省下</span>
          </div>
          <div className="card-sub">
            ≈ ￥{savedYuan.toFixed(2)} · 缓存命中率 <b data-testid="cache-rate">{cacheHitRate}%</b>
          </div>
          <div className="card-foot">创作请求 {fmt(creationReqs)} 次</div>
        </section>

        <section className="big-card card-guard" aria-label="合规引擎护栏">
          <div className="card-eyebrow">合规引擎 · 护栏</div>
          <div className="card-metric" data-testid="guard-blocks">
            {fmt(guardrailBlocks)}
            <span className="card-unit">次拦截</span>
          </div>
          <div className="card-sub">
            审计条目 <b>{fmt(complianceReqs)}</b> 条
          </div>
          <div className="card-foot">守住不下架红线</div>
        </section>
      </div>

      <button className="details-toggle" onClick={() => setOpen((v) => !v)} aria-expanded={open}>
        {open ? '收起明细 ▲' : '展开明细（路由 / 延迟 / 日志）▼'}
      </button>

      {open && (
        <div className="details">
          <div className="detail-row">
            <span>平均延迟</span>
            <b>{avgLatency} ms</b>
          </div>
          <div className="detail-row">
            <span>应答模型分布</span>
            <span className="models">
              {summary && Object.keys(summary.modelsUsed).length > 0
                ? Object.entries(summary.modelsUsed).map(([m, c]) => (
                    <span key={m} className="model-chip">
                      {m} ×{c}
                    </span>
                  ))
                : '—'}
            </span>
          </div>
          <div className="log">
            <div className="log-head">最近请求</div>
            {recentEvents.length === 0 && <div className="log-empty">暂无请求</div>}
            {recentEvents.slice(0, 12).map((e, i) => (
              <div key={`${e.ts}-${i}`} className={`log-item ${e.guardrailBlocked ? 'blocked' : ''}`}>
                <span className="log-step">{e.stepId}</span>
                <span className="log-model">{e.model}</span>
                {e.cacheHit && <span className="tag tag-cache">缓存命中</span>}
                {e.tokensSaved > 0 && <span className="tag tag-save">省 {e.tokensSaved}</span>}
                {e.guardrailBlocked && <span className="tag tag-block">🛡️ 拦截</span>}
                <span className="log-lat">{e.latencyMs}ms</span>
              </div>
            ))}
          </div>
        </div>
      )}
    </aside>
  );
}
