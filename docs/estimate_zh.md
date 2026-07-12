# aegisctl estimate — 接入前省钱测算器

在你**还没安装** AegisGate / **还没调用任何 API** 之前，就能拿到"接入后预计能省多少钱"的具体数字。

> 🇬🇧 **English：** [estimate.md](estimate.md)

---

## 为什么先做估算

任何 AI 成本优化工具都会被问同一个问题：*"花一小时试试值得吗？"* `aegisctl estimate` 用 5 秒钟回答这个问题，用的是和 AegisGate 上线后实际口径**完全一致**的数学。

测算器从本地 `config/models.yaml` 读取模型价格，套用三层节省（缓存命中 / 模型路由 / 提示压缩），打印月度 + 年度节省数字。**它不调用任何 API，也不读取你的审计日志。**

---

## 最简调用

```bash
# 把数字换成你自己的用量，默认按 balanced 工作负载估算
aegisctl estimate \
  --model gpt-4o \
  --monthly-calls 100000 \
  --avg-input-tokens 800 \
  --avg-output-tokens 200
```

样例输出：

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

## 四个真实场景

### 场景 1 — "我在评估 AegisGate，先给我一个数字"

对月度流量有大致估计，但还没装任何东西。默认的 `balanced` 场景采用行业基线的缓存命中率，给出有依据的预测。

```bash
aegisctl estimate \
  --model gpt-4o \
  --monthly-calls 50000 \
  --avg-input-tokens 1000 \
  --avg-output-tokens 250
```

### 场景 2 — "对比两个模型"

想知道单纯从 `gpt-4o` 切到 `gpt-4o-mini` 能省多少？显式指定目标模型并把其他层关掉：

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

### 场景 3 — "我的负载比平均更重复 / 更独特怎么办"

用三档预设：

| 预设 | 缓存命中 | 路由 | 压缩 | 适用场景 |
|---|---|---|---|---|
| `conservative` | 15% | 10% | 5% | 独特 prompt / 单租户 |
| `balanced`（默认）| 30% | 20% | 10% | 多用户 / 通用问答 |
| `aggressive` | 50% | 40% | 15% | 高重复 chatbot / 客服 |

```bash
aegisctl estimate --scenario aggressive \
  --model gpt-4o --monthly-calls 100000 \
  --avg-input-tokens 800 --avg-output-tokens 200
```

### 场景 4 — "生成 JSON 报告供我用 Excel / Python 分析"

```bash
aegisctl estimate \
  --model gpt-4o --monthly-calls 100000 \
  --avg-input-tokens 800 --avg-output-tokens 200 \
  --output json > my-savings-estimate.json
```

JSON schema 详见 `docs/specs/2026-05-26-mvp2-aegisctl-estimate-design.md` §4.3。

---

## 计算公式

公式与 AegisGate 上线后 `SavingsAggregator` 实际记录的三类节省**严格一致**，让估算口径和后台 dashboard 口径完全可对照。

```
baseline = monthly_calls × (avg_input_tokens × P_in + avg_output_tokens × P_out) / 1000

# 缓存：命中即从 CacheStore 直接返回，上游成本为 0
cache_saved = baseline × cache_hit_rate

# 路由：仅作用于未命中部分，切到更便宜的同 provider 模型
non_cached = monthly_calls × (1 - cache_hit_rate)
per_call_diff = (per_call_cost_source) - (per_call_cost_target)   # >= 0
routing_saved = non_cached × routing_savings_rate × per_call_diff

# 压缩：仅作用于未命中部分的 input tokens
non_cached_input_cost = non_cached × avg_input_tokens × P_in / 1000
compression_saved = non_cached_input_cost × compression_rate

total_saved = cache_saved + routing_saved + compression_saved
```

`P_in` / `P_out` 直接来自 `config/models.yaml`。如果你在该文件里加入了自定义 provider，estimate 会自动识别。

---

## 常见问题

### 估算结果偏高 / 偏低怎么办？

默认 `balanced` 预设面向通用工作负载。如果你的 prompt 大多独特（低重复率），用 `conservative`；如果是面向小范围产品的 Q&A 机器人，`aggressive` 更接近。也可以单独覆盖每个 rate：`--cache-hit-rate` / `--routing-savings-rate` / `--compression-rate`。

### 公司还没有用量数据

取你能接受的最低月调用数，配合 `conservative`，结果可视为下限——同模型情况下你的实际节省不会比这更低。

### 为什么缓存命中算 100% 节省？

AegisGate 的 SemanticCache 命中时，根本不调用上游模型，输入和输出 tokens 都是 $0。`cache_hit_rate` 控制的是命中**频率**，不是单次命中节省的比例。

### 如何用 DeepSeek / 通义 / 豆包估算？

`config/models.yaml` 自带主流国内 provider 的价格（deepseek-chat / qwen-plus / doubao-pro-32k / ...）。直接传 model id 即可：

```bash
aegisctl estimate --model deepseek-chat --monthly-calls 100000 \
  --avg-input-tokens 800 --avg-output-tokens 200
```

### conservative / balanced / aggressive 怎么选？

把三档都跑一遍，分别作为悲观 / 中性 / 乐观预测。真实值通常落在 `balanced ± 30%`。

### 为什么路由只作用于未命中部分？

缓存命中的请求根本没有上游调用，无路可"重路由"。路由仅对实际打到模型的请求生效。

### 输出里的 "annualized" 是什么意思？

简单的 `monthly_savings × 12`。不假设复利、不假设增长。

### JSON schema 在哪里？

`docs/specs/2026-05-26-mvp2-aegisctl-estimate-design.md` §4.3。

### 如何用真实数据验证估算？

跑 [5 分钟 quickstart](quickstart_zh.md)，让真实流量经 AegisGate 24 小时，然后 `curl http://localhost:8080/admin/api/savings/summary`。返回的三类节省口径与本工具完全一致，可直接对比。

### 加 `--explain` 会改变数字吗？

不会。`--explain` 仅在表格末尾追加假设来源脚注，数学完全相同。

---

## 已知局限

- **v1 仅支持单模型估算**。多模型批量（`--scenarios scenarios.yaml`）已规划为 TASK-W33。
- **`target_model` 自动选择仅看成本**。自动选择算法用 `(P_in + 3 × P_out) / 4` 加权成本最低；不考虑质量差异——介意质量请显式 `--target-model`。
- **价格表是静态的**。`config/models.yaml` 在安装时定型；provider 改价后请手动更新。

---

**Want to verify?** 跑 [5 分钟 quickstart](quickstart_zh.md)，让真实流量进 AegisGate 24 小时，再对比 `/admin/api/savings/summary`。
