import { useState } from 'react';
import type { RunStreamHandlers } from '../api';
import type { RunStepResult, ScenarioSummary, UiInputSpec, UiStep } from '../types';

interface DeskProps {
  scenario: ScenarioSummary;
  onRunStream: (
    stepId: string,
    inputs: Record<string, unknown>,
    handlers: RunStreamHandlers
  ) => Promise<void>;
}

function initialInputs(step: UiStep): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const input of step.inputs ?? []) {
    if (input.default !== undefined) out[input.name] = input.default;
    else out[input.name] = input.type === 'number' ? 0 : '';
  }
  return out;
}

function InputField({
  spec,
  value,
  onChange,
}: {
  spec: UiInputSpec;
  value: unknown;
  onChange: (v: unknown) => void;
}) {
  const common = { id: spec.name, 'aria-label': spec.label, required: spec.required };
  if (spec.type === 'textarea') {
    return (
      <textarea
        {...common}
        className="field"
        rows={4}
        placeholder={spec.placeholder}
        value={String(value ?? '')}
        onChange={(e) => onChange(e.target.value)}
      />
    );
  }
  if (spec.type === 'select') {
    return (
      <select {...common} className="field" value={String(value ?? '')} onChange={(e) => onChange(e.target.value)}>
        {(spec.options ?? []).map((o) => (
          <option key={o.value} value={o.value}>
            {o.label}
          </option>
        ))}
      </select>
    );
  }
  return (
    <input
      {...common}
      className="field"
      type={spec.type === 'number' ? 'number' : 'text'}
      placeholder={spec.placeholder}
      value={String(value ?? '')}
      onChange={(e) => onChange(spec.type === 'number' ? Number(e.target.value) : e.target.value)}
    />
  );
}

function StepCard({
  scenario,
  step,
  onRunStream,
}: {
  scenario: ScenarioSummary;
  step: UiStep;
  onRunStream: DeskProps['onRunStream'];
}) {
  const [inputs, setInputs] = useState<Record<string, unknown>>(() => initialInputs(step));
  const [result, setResult] = useState<RunStepResult | null>(null);
  const [streamText, setStreamText] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const blocked = result?.guardrail?.blocked;
  // 流式进行中优先展示增量文本；完成后以最终结果为准。
  const displayText = result ? result.output : streamText;

  async function run() {
    setLoading(true);
    setError(null);
    setResult(null);
    setStreamText('');
    try {
      await onRunStream(step.id, inputs, {
        onToken: (text) => setStreamText((prev) => prev + text),
        onDone: (r) => setResult(r),
        onError: (message) => setError(message),
      });
    } catch (e) {
      setError(e instanceof Error ? e.message : '运行失败');
    } finally {
      setLoading(false);
    }
  }

  return (
    <article className={`step-card engine-${step.engine}`}>
      <header className="step-head">
        <span className={`engine-badge ${step.engine}`}>
          {step.engine === 'creation' ? '创作引擎' : '合规引擎'}
        </span>
        <h3>{step.label}</h3>
      </header>
      {step.description && <p className="step-desc">{step.description}</p>}

      <div className="step-inputs">
        {(step.inputs ?? []).map((spec) => (
          <label key={spec.name} className="field-label">
            <span>{spec.label}</span>
            <InputField spec={spec} value={inputs[spec.name]} onChange={(v) => setInputs((s) => ({ ...s, [spec.name]: v }))} />
          </label>
        ))}
      </div>

      <button
        className="run-btn"
        style={{ background: scenario.meta.accentColor }}
        disabled={loading}
        onClick={run}
      >
        {loading ? '运行中…' : '运行此步骤'}
      </button>

      {error && <div className="step-error">⚠️ {error}</div>}

      {(result || displayText) && (
        <div className={`step-output ${blocked ? 'blocked' : ''}`}>
          {blocked && <div className="block-banner">🛡️ 已被护栏拦截：{result?.guardrail?.reason}</div>}
          <pre>
            {displayText}
            {loading && !result && <span className="stream-caret">▍</span>}
          </pre>
          {result && result.toolInvocations.length > 0 && (
            <div className="tool-trace">
              工具调用：
              {result.toolInvocations.map((t, i) => (
                <span key={i} className={`tool-chip ${t.ok ? '' : 'fail'}`}>
                  {t.name}
                </span>
              ))}
            </div>
          )}
        </div>
      )}
    </article>
  );
}

export function GuidedDesk({ scenario, onRunStream }: DeskProps) {
  return (
    <section className="guided-desk" aria-label="引导式创作台">
      <div className="desk-head">
        <h2>{scenario.meta.name}</h2>
        <span className="desk-model">主模型 {scenario.model.primary}{scenario.model.fallback ? ` · 备 ${scenario.model.fallback}` : ''}</span>
      </div>
      <div className="steps">
        {scenario.uiSteps.map((step) => (
          <StepCard key={step.id} scenario={scenario} step={step} onRunStream={onRunStream} />
        ))}
      </div>
    </section>
  );
}
