# AegisGate Showcase App (Reference Demo)

> LLM → **AegisGate** → App: a runnable reference demo that makes the AI gateway's **cost savings** and **compliance** value *immediately visible*.

English | [简体中文](./README_zh.md)

## What is this

A demo app built on a **generic skeleton + pluggable scenario plugins**, routing every LLM call through AegisGate:

- **Flagship · AI Comic-Drama Studio**: killer dual engines — a **creation engine** (batch outline/script/storyboard generation, optimized for semantic cache + token savings) and a **compliance engine** (one-click pre-publish review, optimized against takedowns/bans).
- **Validation · E-commerce Shopping Assistant**: the same plugin interface, different domain — proving the skeleton is generic (zero changes to runtime/routes).

Four gateway values are surfaced in the UI: 🛡️ guardrail blocks, 💰 semantic-cache savings, 🔀 smart routing (responding model visible), 📊 observability aggregation.

## Architecture

```
React frontend (guided studio + dual-core value panel)
        │  /api (BFF only; frontend never holds the gateway key)
        ▼
Express backend (BFF)
  ├─ scenario registry + Function-Calling runtime (each round via gateway, extracts value events)
  ├─ AegisGate client (fetch, captures response headers: tokens-saved / cache / responding model)
  └─ observability aggregation (savings / cache-hit-rate / guardrail blocks / per-engine)
        │  OpenAI-compatible /v1/chat/completions (Bearer AegisGate key)
        ▼
AegisGate gateway (guardrails / cache / routing / observability; provider keys live here)
        ▼
Real LLMs (OpenAI / Claude / Qwen ...)
```

## Quick Start

### Prerequisites
- A running AegisGate gateway (`docker compose up -d` at the repo root, with provider keys configured on the gateway side).
- Node.js ≥ 22 (for manual dev mode).

### Option A: Docker
```bash
# 1. Start the gateway (repo root)
docker compose up -d

# 2. Configure demo env
cp apps/showcase/.env.example apps/showcase/.env
# edit .env, set AEGISGATE_API_KEY (issued by the gateway)

# 3. Start the demo
cd apps/showcase && docker compose up --build
# open http://localhost:5173
```

### Option B: Manual dev
```bash
cp apps/showcase/.env.example apps/showcase/.env   # set AEGISGATE_API_KEY

cd apps/showcase/backend && npm install && npm run dev   # :8090
cd apps/showcase/frontend && npm install && npm run dev   # :5173 (proxies /api → :8090)
```

## Value Demo Script (4 steps)
1. Pick "AI Comic-Drama Studio" → **Generate episode outline** (first time, no cache).
2. Generate again with a similar theme / reuse character bible → **cache hit, savings counter ticks up**.
3. Switch to **One-click compliance review**, paste boundary-violating text (e.g. graphic violence) → **🛡️ blocked + audit entry +1**.
4. Switch primary/fallback model (`SHOWCASE_MODEL` / `SHOWCASE_FALLBACK_MODEL` in `.env`) → the panel's "model distribution" shows the routing change.

## Add a Scenario Plugin

Declare the same contract under `backend/src/scenarios/<your>/` — zero changes to runtime/routes/frontend:

```ts
export function yourPlugin(model): ScenarioPlugin {
  return {
    id: 'your',
    meta: { name, description, icon, accentColor },
    systemPrompt: '...',
    tools: [/* ToolDefinition: spec + pure-function handler (SR-3: no side effects) */],
    uiSteps: [/* generate (model) / tool (direct) / guard (compliance pre-check) */],
    dataset: yourDataset,
    guardrail: { ruleFile: 'your.yaml' },
    model,
  };
}
```
Register it in `backend/src/scenarios/index.ts`; put guardrail rules in `apps/showcase/config/rules/your.yaml` (reusing the gateway's `regex_match` / `keyword_contains` / `length_check` + `block` / `warn` / `modify` format) and mount them on the gateway.

## Environment Variables
| Variable | Description |
|----------|-------------|
| `AEGISGATE_BASE_URL` | Gateway URL (default `http://localhost:8080`) |
| `AEGISGATE_API_KEY` | Demo key issued by the gateway (**backend only**) |
| `SHOWCASE_MODEL` | Primary model (must support Function-Calling) |
| `SHOWCASE_FALLBACK_MODEL` | Fallback model (routing/degradation demo, optional) |
| `PORT` | Backend port (default 8090) |
| `SHOWCASE_CORS_ORIGIN` | Allowed frontend origin |

## Security Invariants
- **SR-1**: Provider keys live on the AegisGate side; the demo only holds the AegisGate key, **backend-only**; the frontend never touches any key.
- **SR-2**: The backend validates all request inputs (zod); invalid/over-limit requests get 4xx.
- **SR-3**: Tool handlers are pure functions — no filesystem writes / command execution / arbitrary network side effects.
- **SR-4**: Guardrail blocks return only the reason/code, never echoing the blocked original text.

## Tests
```bash
cd apps/showcase/backend && npm test    # runtime / client / observability / scenarios / security SRs
cd apps/showcase/frontend && npm test   # value panel / app interaction
```

> Note: because of `real_only` (real LLMs only), end-to-end is a **manual acceptance** step (needs a real gateway + provider keys) and is not in CI; unit tests cover logic via an injectable AegisGate client seam.
