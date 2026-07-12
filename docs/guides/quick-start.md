# AegisGate Quick Start

This guide helps you build, configure, and make your first API call on your machine. The default gateway address is `http://127.0.0.1:8080` (aligned with `server.port` in `config/aegisgate.yaml`).

## Prerequisites

- **Compiler**: C++17 support (GCC 11+ or Clang 14+)
- **CMake**: 3.20 or newer
- **[vcpkg](https://github.com/microsoft/vcpkg)**: Used to fetch Drogon, yaml-cpp, RE2, and other dependencies (manifest mode via `vcpkg.json` at the repository root)

Optional:

- **ONNX semantic embeddings**: Requires separately downloading ONNX Runtime and models; see “Optional: ONNX Neural Embedder” in the project root `README.md`. When not enabled, the gateway can fall back to hash-based embeddings.

## Clone and build

```bash
git clone https://github.com/privonyx/loong-aegisgate.git
cd aegisgate

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Build outputs:

- Gateway: `build/src/aegisgate` (or your chosen `build` directory)
- Admin CLI: `build/aegisctl` (if the target is enabled)

Run tests (optional):

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure
```

## Minimal configuration

Main config: `config/aegisgate.yaml`  
Models and upstreams: `config/models.yaml` (path set by `models_config` in the main config)

**1. Set the gateway API key (environment variable recommended)**

```bash
export AEGISGATE_API_KEY="sk-your-gateway-key"
```

In `aegisgate.yaml`, keep:

```yaml
auth:
  enabled: true
  api_keys:
    - "${AEGISGATE_API_KEY}"
```

**2. Keep at least one provider in `config/models.yaml`**

For example, OpenAI only (put secrets in environment variables; do not commit them):

```yaml
providers:
  - name: openai
    type: openai
    base_url: "https://api.openai.com/v1"
    api_keys:
      - key: "${OPENAI_API_KEY}"
        weight: 1
    models:
      - id: "gpt-4o-mini"
        cost_per_1k_input: 0.00015
        cost_per_1k_output: 0.0006
        max_tokens: 128000
    timeout_ms: 30000
    max_retries: 2
```

```bash
export OPENAI_API_KEY="sk-..."
```

**3. (Optional) Admin key**

Required for `/admin/reload`, `/admin/logs/stream`, and similar:

```yaml
auth:
  admin_key: "${AEGISGATE_ADMIN_KEY}"
```

## Start the gateway

```bash
./build/src/aegisgate config/aegisgate.yaml
```

Verify health:

```bash
curl -s http://127.0.0.1:8080/health | jq .
```

## First chat call (curl)

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gpt-4o-mini",
    "messages": [{"role": "user", "content": "Describe AegisGate in one sentence"}],
    "max_tokens": 256
  }' | jq .
```

List models:

```bash
curl -s http://127.0.0.1:8080/v1/models \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" | jq .
```

## Official SDKs

### Python

```bash
pip install -e /path/to/aegisgate/sdk/python
# or: uv pip install -e ...
```

```python
from aegisgate import AegisGateClient

client = AegisGateClient(
    api_key="sk-your-gateway-key",
    base_url="http://127.0.0.1:8080",
)
r = client.chat("Hello", model="gpt-4o-mini")
print(r.content)
```

### Node.js

```bash
npm install @aegisgate/sdk
```

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({
  apiKey: 'sk-your-gateway-key',
  baseUrl: 'http://127.0.0.1:8080',
});
const res = await client.chat('Hello', { model: 'gpt-4o-mini' });
console.log(res.content);
```

### Go

```bash
go get github.com/privonyx/loong-aegisgate-go
```

```go
client := aegisgate.NewClient(
    aegisgate.WithBaseURL("http://127.0.0.1:8080"),
    aegisgate.WithAPIKey("sk-your-gateway-key"),
)
resp, err := client.ChatCompletions(ctx, &aegisgate.ChatCompletionsRequest{
    Model: "gpt-4o-mini",
    Messages: []aegisgate.Message{{Role: "user", Content: "Hello"}},
})
```

## CLI overview (aegisctl)

```bash
export AEGISGATE_URL="http://127.0.0.1:8080"
export AEGISGATE_API_KEY="sk-your-gateway-key"

./build/aegisctl health
./build/aegisctl models
./build/aegisctl chat "ping"
./build/aegisctl config validate config/aegisgate.yaml
```

Admin endpoints require the **admin** key: `./build/aegisctl --api-key "$AEGISGATE_ADMIN_KEY" logs tail`.

## Smart routing (optional)

### Enable ML routing

Add to `config/aegisgate.yaml`:

```yaml
routing:
  type: ml                # basic | cost_aware | ml
  ml:
    cost_weight: 0.4      # cost weight
    quality_weight: 0.35  # quality weight
    latency_weight: 0.25  # latency weight
```

ML routing picks the best model dynamically from actual cost, historical success rate, and response latency.

### Enable A/B testing

Compare cost-effectiveness across two models:

```yaml
routing:
  type: cost_aware
  ab_tests:
    - name: "gpt4o-vs-deepseek"
      variants:
        - model: "gpt-4o-mini"
          weight: 50
        - model: "deepseek-chat"
          weight: 50
      enabled: true
```

When a request omits the `model` field, 50% of traffic goes to gpt-4o-mini and 50% to deepseek-chat. You can compare assignment outcomes in the cost analytics dashboard.

### Using quality_tier

Clients can request a model tier via the `extra` field:

```bash
# Economy mode: pick the cheapest available model
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [{"role": "user", "content": "Hello"}],
    "extra": {"quality_tier": "economy"}
  }' | jq .

# Premium mode: pick the most expensive (typically highest quality) model
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [{"role": "user", "content": "Write a professional analysis report"}],
    "extra": {"quality_tier": "premium"}
  }' | jq .
```

## Reference demo app (Showcase)

Want to see the full picture — **LLM → AegisGate → app**? The repo ships a runnable reference demo under [`apps/showcase`](../../apps/showcase/README.md) that makes the gateway's value (🛡️ guardrails, 💰 semantic-cache savings, 🔀 smart routing, 📊 observability) visible at a glance, with an AI comic-creation studio as the flagship scenario.

**Prerequisites**

- A running AegisGate gateway (see [Start the gateway](#start-the-gateway) above), with provider keys configured on the gateway side.
- Node.js ≥ 22 (only for the manual dev mode).

**Run it (Docker)**

```bash
cp apps/showcase/.env.example apps/showcase/.env
# Edit .env and fill in AEGISGATE_API_KEY (issued by the gateway)
cd apps/showcase && docker compose up --build
# Open http://localhost:5173
```

**Run it (manual dev)**

```bash
cp apps/showcase/.env.example apps/showcase/.env   # fill in AEGISGATE_API_KEY

cd apps/showcase/backend && npm install && npm run dev    # :8090
cd apps/showcase/frontend && npm install && npm run dev   # :5173 (proxies /api -> :8090)
```

The demo only ever holds the AegisGate key (never any provider key), and the frontend never touches a key. For the value walkthrough, scenario plugins, environment variables, and security notes, see the [Showcase README](../../apps/showcase/README.md).

## Data, config & upgrades (distribution package)

> This section targets deployments from the distribution package (`tar.gz`) produced by `scripts/package.sh`. The path notes apply to source builds too.

**Default locations for data and logs (all relative to the working directory, configurable in `config/aegisgate.yaml`)**

| Item | Default path | Config key |
|---|---|---|
| SQLite database | `data/aegisgate.db` | `storage.sqlite.path` |
| Application log | `logs/aegisgate.log` | `logging.file` |
| Audit log | `logs/audit.log` | `audit.log_path` |

`data/`, `logs/`, and `models/` are created/downloaded on demand at runtime and are **not shipped in the package**, so they are never overwritten on upgrade.

**Config files (seeded on first run, never overwritten on upgrade)**

The package ships config as `*.example` factory templates (e.g. `config/aegisgate.yaml.example`, `config/models.yaml.example`, `config/rules/*.yaml.example`). On first start, `start.sh` generates each **missing** real config from its template; existing ones are left untouched. The real config (`config/*.yaml`) is **never in the package**.

**Upgrade (extract over in place, zero concerns)**

Just extract the new version over your current deployment directory — no need to delete the old one first:

```bash
tar xzf aegisgate-<version>-<os>-<arch>.tar.gz --strip-components=1 -C <deploy-dir>
```

- Updated: program files such as `aegisgate` / `aegisctl` / `lib/` / `web/` / `scripts/` / `start.sh`, plus the `config/*.example` factory templates.
- Untouched: your `config/*.yaml`, `config/rules/*.yaml`, and `data/`, `logs/`, `models/`.
- To diff against the new defaults: compare `config/aegisgate.yaml` with `config/aegisgate.yaml.example`.

**Running the production profile (Redis + PostgreSQL + OpenTelemetry + Guard)**

The default profile is community (in-memory cache + SQLite), fine for single-host/trial use. If the package was built in the production profile (`scripts/build.sh -t Release`, with Redis/PG/OTel/Guard all compiled in), you can run the **prod profile** — the package already ships `config/aegisgate.prod.yaml.example`:

```bash
# Option 1: flag
./start.sh --profile prod

# Option 2: environment variable
AEGISGATE_PROFILE=prod ./start.sh
```

`start.sh` seeds `config/aegisgate.prod.yaml` on first start (kept if it already exists) and launches with it. Provision Redis/PostgreSQL/OTel Collector first (see `scripts/setup-prod-deps.sh`) and fill in connection details via the config/env.

> ⚠️ **Fail-closed (default behavior):** the production profile defaults to `storage.strict_backends: true` — if the YAML requests `redis`/`postgres` but the binary was not built with that backend, or the backend is unreachable at startup, the process **refuses to start** (non-zero exit + clear log) instead of silently falling back to memory. Set `storage.strict_backends: false` explicitly if you want "degrade-but-stay-up" on backend failure.

Use `scripts/smoke-prod.sh` to verify all backends are truly active in one shot:

```bash
bash scripts/smoke-prod.sh --mode real --cmake-log build/build.log
```

## Next steps

- [Architecture guide](./architecture.md) — system overview, flowcharts, sequence diagrams
- [Cost optimization guide](./cost-optimization.md) — saving strategies in detail
- [Admin API reference](./admin-api.md) — full Admin REST API reference
- [Error codes reference](./error-codes.md) — understanding `AEGIS-xxxx` and retry behavior
- [Security best practices](./security-best-practices.md) — keys, TLS, guardrail rules
- [Performance tuning](./performance-tuning.md) — caching, rate limiting, threads
- [Troubleshooting](./troubleshooting.md) — common startup and upstream issues

For a fuller feature overview, see the project root `README.md`.
