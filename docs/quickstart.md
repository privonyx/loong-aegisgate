# 5-Minute Quickstart

Try AegisGate end-to-end on your laptop in 5 minutes — bring your own OpenAI key,
get a working AI gateway with semantic caching and a Savings dashboard.

> 🇨🇳 **中文：** [quickstart_zh.md](quickstart_zh.md)

---

## Overview

In 5 minutes, you will:

1. Pull and start AegisGate with one `docker run` command
2. Read your auto-generated API key from the startup banner
3. Make your first LLM call (cache miss)
4. Make the same call again (cache hit — instant return)
5. View the Savings dashboard showing `tokens_saved > 0`

---

## Prerequisites

- **Docker** ([install](https://docs.docker.com/get-docker/))
- **OpenAI API key** ([get one](https://platform.openai.com/api-keys))
- **curl** (any recent version)

Optional:

- `jq` for pretty-printing JSON responses
- 5 minutes of attention

---

## Step 1 — Pull and Run (~30 seconds + image pull time)

Build the image locally (first time only):

```bash
git clone https://github.com/privonyx/loong-aegisgate.git
cd aegisgate
docker build -t aegisgate:latest .
```

Set your OpenAI key and run the quickstart container:

```bash
export OPENAI_API_KEY=sk-...your-openai-key-here...

docker run --rm -it \
  --name aegisgate-quickstart \
  -p 8080:8080 \
  -e OPENAI_API_KEY=$OPENAI_API_KEY \
  -v aegisgate-quickstart-data:/app/data \
  --entrypoint /usr/local/bin/quickstart-entrypoint.sh \
  aegisgate:latest
```

> 💡 **Shortcut:** if you cloned the repo, `bash scripts/quickstart.sh` runs the
> same command for you (after `export OPENAI_API_KEY`).

---

## Step 2 — Read Your API Key from the Banner (~5 seconds)

The container prints a banner like this on startup:

```
╔══════════════════════════════════════════════════════════════════╗
║  ⚠️  AegisGate QUICKSTART MODE — development / demo ONLY  ⚠️    ║
║                                                                  ║
║  Quickstart API key (auto-generated):                            ║
║    AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfG
║                                                                  ║
║  Try it now (after server starts on :8080):                      ║
║    curl -H "Authorization: Bearer AbCdEfG..." \                  ║
║         http://localhost:8080/admin/api/savings/summary
║                                                                  ║
║  DO NOT use in production. See docs/quickstart.md                ║
╚══════════════════════════════════════════════════════════════════╝
```

Copy the API key. You can also read it later from the persistent volume:

```bash
docker exec aegisgate-quickstart cat /app/data/quickstart-key.txt
```

Set it in your shell for the curl commands below:

```bash
export QUICKSTART_KEY=AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfG
```

---

## Step 3 — First LLM Call (cache miss, ~1-3 seconds)

Open a second terminal and make your first request:

```bash
time curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Authorization: Bearer $QUICKSTART_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"Say hello in 5 words"}]}'
```

Expected: a normal OpenAI-style response, with latency dominated by OpenAI's
network call (~500ms – 3s typically).

```json
{
  "id": "chatcmpl-...",
  "choices": [{"message": {"role": "assistant", "content": "Hello there, friend of mine!"}}],
  "usage": {"prompt_tokens": 14, "completion_tokens": 7, "total_tokens": 21}
}
```

---

## Step 4 — Second Identical Call (cache hit, <10ms)

Run the **exact same** request again:

```bash
time curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Authorization: Bearer $QUICKSTART_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"Say hello in 5 words"}]}'
```

Expected: **identical response, ~100x faster** (typically <10ms). AegisGate
recognized the semantically identical request and served from the in-memory
semantic cache without hitting OpenAI.

Compare the `time` output of step 3 vs step 4 — that gap is exactly the value
AegisGate delivers.

---

## Step 5 — View Savings Dashboard (~10 seconds)

Query the savings summary:

```bash
curl http://localhost:8080/admin/api/savings/summary \
  -H "Authorization: Bearer $QUICKSTART_KEY" | jq
```

Expected output:

```json
{
  "tokens_saved": 21,
  "cache_hits": 1,
  "cache_misses": 1,
  "hit_rate": 0.5,
  "cost_saved_usd": 0.000004,
  "since_iso": "2026-05-25T13:42:00Z"
}
```

That's it — **5 steps, 5 minutes, real data**. Run more requests to watch the
savings grow.

You can also browse the full Admin UI at `http://localhost:8080/admin/` (use the
same `QUICKSTART_KEY` as Bearer token).

---

## Step 6 — Project Your Savings (~5 seconds, no Docker required)

Already curious how much AegisGate would save you on your real monthly volume?
Run the pre-flight estimator — it doesn't even need Docker:

```bash
# Replace with your own monthly traffic and token sizes
aegisctl estimate \
  --model gpt-4o \
  --monthly-calls 100000 \
  --avg-input-tokens 800 \
  --avg-output-tokens 200
```

Sample output (gpt-4o, 100k calls/mo, balanced scenario):

```
  Cache hits (30%):           -$210.00
  Routing (20% to gpt-4o-mini): -$94.64
  Compression (10% on input):  -$28.00
  Estimated monthly savings:  -$332.64 (47.5%)
  Estimated annual savings:    $3991.68
```

The estimator pulls model prices from `config/models.yaml`, so DeepSeek /
Qwen / Doubao all work out of the box. Try `--scenario conservative` for a
floor estimate, or `--output json` for a machine-readable report.

📖 Full guide: [estimate.md](estimate.md)

---

## Common Issues

### `OPENAI_API_KEY not set` warning

The quickstart will still start (so you can poke around the admin UI), but LLM
calls will fail. Set the env var and restart:

```bash
docker rm -f aegisgate-quickstart
export OPENAI_API_KEY=sk-...
# re-run the docker run command from Step 1
```

### Port 8080 already in use

Map to a different host port:

```bash
docker run ... -p 18080:8080 ... aegisgate:latest
# then use http://localhost:18080
```

### Can't find the auto-generated key in logs

The banner only prints once. Read it directly from the persistent volume:

```bash
docker exec aegisgate-quickstart cat /app/data/quickstart-key.txt
```

### `MUST NOT run in production` error

You set `AEGISGATE_PRODUCTION=1`. The quickstart entrypoint hard-fails as a
safety guard. For production, use the standard entrypoint (don't pass
`--entrypoint`):

```bash
docker run -p 8080:8080 \
  -v $(pwd)/config:/app/config:ro \
  -e AEGISGATE_API_KEY=$(openssl rand -base64 32) \
  aegisgate:latest config/aegisgate.yaml
```

### Cache hit isn't faster

Make sure the second request is **byte-identical** to the first (same model, same
messages, same parameters). The semantic cache key includes all request fields.

---

## From Quickstart to Production

The quickstart is **not** a production deployment. When you're ready to move on:

| Concern | Quickstart | Production |
|---|---|---|
| API key | Auto-generated, dev-only | Strong random key + secret manager |
| Auth | Bearer token only | Add JWT / OIDC / SCIM (see `config/aegisgate.yaml`) |
| Storage | SQLite in container | PostgreSQL (`-DENABLE_PG=ON`) |
| Cache | In-memory | Redis (`-DENABLE_REDIS=ON`) |
| Observability | Basic Prometheus | OpenTelemetry + tracing backend |
| Multi-tenant / RBAC | Disabled | Enabled (see `rbac:` section in full config) |
| Bind address | `0.0.0.0:8080` | Behind reverse proxy + TLS termination |

The quickstart container will **hard-fail** if you set `AEGISGATE_PRODUCTION=1`,
forcing a deliberate switch to the production entrypoint. See the main README's
"Deployment" section for production guidance.

---

## Next Steps

- Read [README.md](../README.md) for the full feature overview
- Browse [docs/](.) for architecture, security, and operations guides
- Install the SDK in your own code:

  ```bash
  # Python
  pip install aegisgate

  # Node.js / TypeScript
  npm install @aegisgate/sdk
  ```

  ```python
  from aegisgate import AegisGateClient
  client = AegisGateClient(api_key="sk-xxx", base_url="http://localhost:8080")
  print(client.chat("Hello").content)
  ```

  ```typescript
  import { AegisGateClient } from '@aegisgate/sdk';
  const client = new AegisGateClient({ apiKey: 'sk-xxx', baseUrl: 'http://localhost:8080' });
  console.log((await client.chat('Hello!')).content);
  ```

  Both packages are published at version `0.1.0` (Beta) — see
  [sdk/python](../sdk/python/) and [sdk/nodejs](../sdk/nodejs/) for full API docs.

- Share your savings story or report issues at
  [GitHub Issues](https://github.com/privonyx/loong-aegisgate/issues)

---

> **Why we built this quickstart:** any AI gateway that takes more than 5
> minutes to evaluate loses users before it can prove its value. AegisGate's
> philosophy is "show me the savings in the first hour, not the first month".
