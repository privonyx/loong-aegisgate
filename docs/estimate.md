# aegisctl estimate — Pre-flight Savings Estimator

Get a concrete number for "how much can AegisGate save me" — **before** you
install or call any API.

> 🇨🇳 **中文：** [estimate_zh.md](estimate_zh.md)

---

## Why estimate first

The hardest question in any AI cost-optimization tool is: *"is it worth my
hour to even try this?"* `aegisctl estimate` answers that in 5 seconds, with
math that matches what AegisGate actually does once you install it.

The estimator pulls model prices from your local `config/models.yaml`, applies
three savings layers (cache hits, model routing, prompt compression) and
prints a monthly + annual savings number. **It never calls any API and never
reads your audit logs.**

---

## Quickest path

```bash
# Replace numbers with your own usage. Defaults assume balanced workload.
aegisctl estimate \
  --model gpt-4o \
  --monthly-calls 100000 \
  --avg-input-tokens 800 \
  --avg-output-tokens 200
```

Sample output:

```
AegisGate Savings Estimate

Input:
  Model:              gpt-4o
  Monthly volume:     100000 calls x (800 in + 200 out tokens)
  Scenario:           balanced

Baseline cost (without AegisGate):
  Total monthly:                                         $    700.00

With AegisGate:
  Cache hits (30%):                                  -$    210.00
  Routing (20% to gpt-4o-mini):                      -$     94.64
  Compression (10% on input):                        -$     28.00

  Estimated monthly savings:                            -$    332.64 (47.5%)
  Estimated annual savings:                              $   3991.68

Want to verify? Run quickstart and check after 24h:
  -> docs/quickstart.md
  -> http://localhost:8080/admin/api/savings/summary
```

---

## Four real-world scenarios

### Scenario 1 — "I'm evaluating AegisGate, give me a number"

You have a rough sense of monthly traffic but haven't installed anything yet.
The default `balanced` scenario assumes industry-baseline cache hit rates and
gives you a defensible projection.

```bash
aegisctl estimate \
  --model gpt-4o \
  --monthly-calls 50000 \
  --avg-input-tokens 1000 \
  --avg-output-tokens 250
```

### Scenario 2 — "Compare two models head to head"

Want to know what switching from `gpt-4o` to `gpt-4o-mini` saves on its own?
Pin the target model and zero out the unused layers:

```bash
aegisctl estimate \
  --model gpt-4o \
  --target-model gpt-4o-mini \
  --monthly-calls 100000 \
  --avg-input-tokens 800 \
  --avg-output-tokens 200 \
  --cache-hit-rate 0 \
  --routing-savings-rate 1.0 \
  --compression-rate 0
```

### Scenario 3 — "What if my workload is more / less repetitive than average?"

Use the three-tier preset:

| Preset | Cache hit | Routing | Compression | Best for |
|---|---|---|---|---|
| `conservative` | 15% | 10% | 5% | Unique prompts, single-tenant |
| `balanced` (default) | 30% | 20% | 10% | Multi-user, general Q&A |
| `aggressive` | 50% | 40% | 15% | High-repeat chatbot, customer service |

```bash
aegisctl estimate --scenario aggressive \
  --model gpt-4o --monthly-calls 100000 \
  --avg-input-tokens 800 --avg-output-tokens 200
```

### Scenario 4 — "Generate a JSON report for my analysis spreadsheet"

```bash
aegisctl estimate \
  --model gpt-4o --monthly-calls 100000 \
  --avg-input-tokens 800 --avg-output-tokens 200 \
  --output json > my-savings-estimate.json
```

The JSON schema is documented under `docs/specs/2026-05-26-mvp2-aegisctl-estimate-design.md` §4.3.

---

## How the math works

Aligned with the three savings classes that AegisGate's `SavingsAggregator`
actually records at runtime, so the estimate vocabulary matches the dashboard
vocabulary exactly.

```
baseline = monthly_calls × (avg_input_tokens × P_in + avg_output_tokens × P_out) / 1000

# Cache: hit means the response is served from CacheStore, zero upstream cost
cache_saved = baseline × cache_hit_rate

# Routing: only on non-cached calls, switching to a cheaper sibling model
non_cached = monthly_calls × (1 - cache_hit_rate)
per_call_diff = (per_call_cost_source) - (per_call_cost_target)  # >= 0
routing_saved = non_cached × routing_savings_rate × per_call_diff

# Compression: only on input tokens of non-cached calls
non_cached_input_cost = non_cached × avg_input_tokens × P_in / 1000
compression_saved = non_cached_input_cost × compression_rate

total_saved = cache_saved + routing_saved + compression_saved
```

`P_in` / `P_out` come straight from `config/models.yaml`. If you list your
own custom provider there, estimate picks it up automatically.

---

## FAQ

### Why does my estimate look too high / too low?

The default `balanced` preset is calibrated for general-purpose workloads.
If your prompts are mostly unique (low repeat rate), use `conservative`;
if you run a Q&A bot on a small set of products, `aggressive` is closer.
Or override each rate individually with `--cache-hit-rate`,
`--routing-savings-rate`, `--compression-rate`.

### My company has no usage data yet

Pick the lowest plausible monthly call number and use `conservative`. The
result is a floor — your actual savings cannot reasonably be lower than
this on the same model.

### Why is cache hit shown as 100% saved?

When AegisGate's SemanticCache returns a hit, the upstream model is never
called. Both input and output tokens are charged $0. The `cache_hit_rate`
controls *how often* that happens, not how much each hit saves.

### How do I estimate for DeepSeek / 通义 / Doubao?

`config/models.yaml` ships with pricing for all major Chinese providers
(deepseek-chat, qwen-plus, doubao-pro-32k, ...). Just pass the model id:

```bash
aegisctl estimate --model deepseek-chat --monthly-calls 100000 \
  --avg-input-tokens 800 --avg-output-tokens 200
```

### How do I choose between conservative / balanced / aggressive?

Run all three and treat them as low / mid / high savings projections.
The truth typically lands within `balanced ± 30%`.

### Why is routing applied only on non-cached calls?

If a call is cache-hit, there's no upstream call to reroute. Routing only
takes effect on the calls that actually go to the model.

### What does "annualized" mean in the output?

Simply `monthly_savings × 12`. It does not assume compounding or growth.

### Where is the JSON schema documented?

`docs/specs/2026-05-26-mvp2-aegisctl-estimate-design.md` §4.3.

### How do I verify the estimate against reality?

Install the [5-minute quickstart](quickstart.md), point your real traffic at
AegisGate for 24 hours, then `curl
http://localhost:8080/admin/api/savings/summary`. The breakdown uses the
same three categories, so direct comparison is apples-to-apples.

### Will adding `--explain` change the numbers?

No. `--explain` only appends the assumption-source footnotes after the
table; the math is identical.

---

## Limitations

- **Single-model only in v1.** Multi-model batch estimation (`--scenarios
  scenarios.yaml`) is on the roadmap as TASK-W33.
- **Cost-only `target_model` selection.** The auto-pick uses
  `(P_in + 3 × P_out) / 4` as the cost metric. Quality differences are not
  modeled — supply `--target-model` explicitly if you care.
- **Static price table.** `config/models.yaml` is captured at install time.
  When provider pricing changes, update the file.

---

**Want to verify?** Run the [5-minute quickstart](quickstart.md), point your
real traffic at AegisGate for 24 hours, then compare with
`/admin/api/savings/summary`.
