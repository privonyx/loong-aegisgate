# MVP-5 Case Study Template

> **用途：** 当 seed user 同意把使用经验写成案例时，maintainer 用本模板写出
> Case Study 博客。本模板与 admin dashboard "Case Study Numbers" 行（Row 4）
> 严格对齐 — 模板中 `<saved vs baseline>` 等占位符可直接从 dashboard
> 复制粘贴。
>
> **任务来源：** TASK-20260527-02（MVP-5 prep / 核心功能与后台可观测细节优化）
> + TASK-20260527-01（MVP-4 Seed User Outreach Toolkit）。

## 0. 写作前的素材检查

| 素材 | 来源 | 是否就绪 |
|------|------|---------|
| 头条 3 数字 | admin dashboard Row 4 截图 / `/admin/api/case-study/headline` JSON | ☐ |
| baseline 假设 | `CostTracker::baselineModel()`（默认首个 setPricing 调用的 model）| ☐ |
| 接入耗时 | seed user 反馈 issue 的 `install_method` + `pain_points` | ☐ |
| 公开同意 | seed_user_feedback issue 的 `case_study_consent` checkbox | ☐ |
| `aegisctl estimate` 数字 | 接触前发给 user 的预估 vs 实测对比 | ☐ |

> **强制 gate：** 5 项中必须 5 项就绪才动笔。少任何一项 → 回到 seed-user-playbook.md
> 第 4 步（Feedback Collection）补齐。

---

## 1. 标题模板

> **写作目标：** B2B 可读 / 可分享到 LinkedIn-like 平台 / 不夸张但有数字震撼。

**主标题（推荐）：**
- `<COMPANY> 用 AegisGate 把 LLM 月度账单从 $X 压到 $Y（节省 Z%）`
- `<COMPANY> + AegisGate: cache hit Y% 撬动 $Z saved vs baseline`

**避免：**
- ❌ "革命性..."、"前所未有..."（过度营销 / 反 dark-pattern）
- ❌ "100% safe"、"零风险"（虚假保证 / SR3 风格）

---

## 2. 主体结构（5 段，~600-900 字）

### §1 Background（~100 字）
- 一句话介绍 `<COMPANY>`：行业 / 规模 / LLM 使用场景
- 接入 AegisGate 之前的痛点（直接引用 seed_user_feedback 的 `pain_points`）

### §2 The "saved vs baseline" Number（~150 字 / 头条 1）

> 直接引用 dashboard Row 4 第一张卡。

```
$<saved vs baseline>  saved
节省 <savings_percent>%（baseline = $<baseline_cost>）
```

- 说清 baseline 是哪个模型（如 GPT-4 直连）
- 说清 actual 走的是哪条路径（cache hit / 路由到便宜模型 / 等）
- **严禁夸大：** 如果窗口短（< 7 天），明确说"7 天试运行数据 / 月度推算"

### §3 The "cache hit" Y% Number（~150 字 / 头条 2）

> 引用 dashboard Row 4 第二张卡。

```
cache hit <total_hit_rate>%
  exact <hit_exact> / semantic <hit_semantic> / conversation <hit_conversation>
```

- 解释三档命中类型（一句话 / 帮助读者理解为什么不是简单的 LRU 缓存）
- 强调 `aegisctl estimate` 在接入前的预估 vs 实际命中率对比

### §4 The "quality" Number（~150 字 / 头条 3）

> 引用 dashboard Row 4 第三张卡。

```
quality EMA <current_ema>  (slope <slope>)
  factuality <reason_factuality> / refusal <reason_refusal> / latency <reason_latency_degraded>
```

- **不要堆术语：** 用一句话说"质量没掉"（slope ≥ 0）或"轻微下降但可接受"
- 如果有 reason 计数，说清楚是哪一档触发最多 → 暗示后续优化方向

### §5 The Path Forward（~100 字）
- 1-2 句 `<COMPANY>` 接下来计划怎么扩大使用范围
- 提及 ADOPTERS.md，让其他人来 PR 加入 list
- 链接到 GitHub repo + `<COMPANY>` 自家的工程博客（如有）

---

## 3. 数字使用纪律

> **保护信任：dashboard Row 4 数字是 case study 的"硬证据"。一旦造假，整个
> case study program 就废了。**

### 3.1 必须做

- 截图来自真实 admin dashboard（带时间戳）
- 数字范围与 seed user issue 中 `actual_savings_pct` 字段一致（差异 < 5%）
- 如果是月度推算，明确写"基于 7 天数据 ×4.3"

### 3.2 严禁做

- ❌ 把 v2 backlog 的 per-tenant 数字 mix 进 v1 进程级数字
- ❌ 隐去 baseline_cost / 只展示 saved（让读者无法核算）
- ❌ 用 `<COMPANY>` logo 但内容并非 `<COMPANY>` 实际授权的（必须有
  `case_study_consent` checkbox 同意）

---

## 4. 文末标准段（可直接复用）

```markdown
---

## About AegisGate

AegisGate is an open-core LLM gateway that routes, caches, and audits your
LLM calls. Free for community use, with enterprise edition available.

- **GitHub:** https://github.com/privonyx/loong-aegisgate
- **Adopters list:** [ADOPTERS.md](../../ADOPTERS.md)
- **Try it:** `aegisctl estimate --prompt "your prompt"` 跑一次预估，
  数字直接来自我们的 pricing table 与 cache 模型。

If your team uses AegisGate, consider opening a PR to ADOPTERS.md or filing
a "[Used AegisGate? Tell us](https://github.com/privonyx/loong-aegisgate/issues/new?template=seed_user_feedback.yml)"
issue — your story might become the next case study like this one.
```

---

## 5. 发布前 review checklist

- [ ] dashboard 截图 3 张（Row 4 三张卡分别截图，且时间戳一致）
- [ ] 头条数字 3 个全部对应 dashboard 实际值（误差 < 0.1%）
- [ ] seed_user_feedback issue 链接已嵌入（透明可追溯）
- [ ] `<COMPANY>` 已通过 case_study_consent + 邮件二次确认
- [ ] 不含任何"100% safe / 零风险 / 革命性"词汇（dark-pattern 防御）
- [ ] 不暴露任何敏感数据（API key / model fine-tune 细节 / 客户身份）
- [ ] 文末包含上述"About AegisGate"标准段
- [ ] LinkedIn-like 平台先发草稿给 maintainer + `<COMPANY>` 双 review
