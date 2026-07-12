import { useCallback, useEffect, useState } from 'react';
import { getScenarios, getSummary, runStepStream, type RunStreamHandlers } from './api';
import { GuidedDesk } from './components/GuidedDesk';
import { ScenarioSelector } from './components/ScenarioSelector';
import { ValuePanel } from './components/ValuePanel';
import type { ObservabilitySummary, ScenarioSummary, ValueEvent } from './types';

export function App() {
  const [scenarios, setScenarios] = useState<ScenarioSummary[]>([]);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [summary, setSummary] = useState<ObservabilitySummary | null>(null);
  const [recentEvents, setRecentEvents] = useState<ValueEvent[]>([]);
  const [loadError, setLoadError] = useState<string | null>(null);

  useEffect(() => {
    getScenarios()
      .then((list) => {
        setScenarios(list);
        if (list.length > 0) setSelectedId(list[0].id);
      })
      .catch((e) => setLoadError(e instanceof Error ? e.message : '加载场景失败'));
    getSummary().then(setSummary).catch(() => undefined);
  }, []);

  const refreshSummary = useCallback(() => {
    getSummary().then(setSummary).catch(() => undefined);
  }, []);

  const handleRunStream = useCallback(
    async (stepId: string, inputs: Record<string, unknown>, handlers: RunStreamHandlers): Promise<void> => {
      if (!selectedId) throw new Error('未选择场景');
      await runStepStream(selectedId, stepId, inputs, {
        onToken: handlers.onToken,
        onValue: (event) => setRecentEvents((prev) => [event, ...prev].slice(0, 50)),
        onDone: (result) => {
          handlers.onDone?.(result);
          refreshSummary();
        },
        onError: handlers.onError,
      });
    },
    [selectedId, refreshSummary]
  );

  const selected = scenarios.find((s) => s.id === selectedId) ?? null;

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="logo">🛡️</span>
          <div>
            <div className="brand-title">AegisGate Showcase</div>
            <div className="brand-sub">大模型 → AegisGate → 应用：省钱与合规，肉眼可见</div>
          </div>
        </div>
        <a className="dash-link" href="http://localhost:8080" target="_blank" rel="noreferrer">
          AegisGate Dashboard ↗
        </a>
      </header>

      {loadError && (
        <div className="banner-error">
          无法连接后端：{loadError}。请确认已启动 showcase 后端（默认 :8090）与 AegisGate 网关。
        </div>
      )}

      <ScenarioSelector scenarios={scenarios} selectedId={selectedId} onSelect={setSelectedId} />

      <main className="layout">
        {selected ? (
          <GuidedDesk scenario={selected} onRunStream={handleRunStream} />
        ) : (
          <section className="guided-desk empty">{loadError ? '后端未就绪' : '加载中…'}</section>
        )}
        <ValuePanel summary={summary} recentEvents={recentEvents} />
      </main>
    </div>
  );
}
