import type { ScenarioSummary } from '../types';

interface Props {
  scenarios: ScenarioSummary[];
  selectedId: string | null;
  onSelect: (id: string) => void;
}

export function ScenarioSelector({ scenarios, selectedId, onSelect }: Props) {
  return (
    <nav className="scenario-tabs" aria-label="场景选择">
      {scenarios.map((s) => (
        <button
          key={s.id}
          className={`scenario-tab ${selectedId === s.id ? 'active' : ''}`}
          style={selectedId === s.id ? { borderColor: s.meta.accentColor, color: s.meta.accentColor } : undefined}
          onClick={() => onSelect(s.id)}
          aria-pressed={selectedId === s.id}
        >
          <span className="tab-name">{s.meta.name}</span>
          <span className="tab-desc">{s.meta.description}</span>
        </button>
      ))}
    </nav>
  );
}
