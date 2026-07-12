# AegisGate 成本优化指南

本文档详细介绍 AegisGate 的成本优化能力链条，帮助团队将 AI API 调用成本降低 30–60%。

## 成本优化总览

AegisGate 通过 5 层机制协同省钱：

```
┌─────────────────────────────────────────────────────────────────┐
│                   AegisGate 省钱能力栈                          │
│                                                                 │
│  第 1 层 ─ 语义缓存          相同/相似请求零成本复用             │
│  第 2 层 ─ 安全护栏拦截      恶意请求零 Token 消耗               │
│  第 3 层 ─ 智能路由          成本/质量/延迟三维最优选择           │
│  第 4 层 ─ 租户成本限额      日/月硬上限，超限自动拒绝           │
│  第 5 层 ─ 用量预测          趋势预估 + 预算耗尽告警             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 第 1 层：语义缓存

### 工作原理

```
请求 "Python是什么编程语言？"
        │
        ▼
┌──────────────────┐
│ Embedder 向量化   │  将文本转为向量表示
│ (Hash / ONNX)    │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ hnswlib ANN 检索 │  在向量索引中搜索最近邻
└────────┬─────────┘
         │
    相似度 ≥ 阈值?
    ┌────┴────┐
   YES       NO
    │         │
    ▼         ▼
 返回缓存   调用模型
(~1ms)    (~2-5s)
 零成本    产生Token费用
```

### 配置建议

```yaml
cache:
  threshold: 0.90          # 相似度阈值（越低命中率越高，但可能不精确）
  ttl_seconds: 3600        # 缓存有效期
  max_entries: 10000       # 最大缓存条目
  context_aware: true      # 多轮对话感知

  # 自适应阈值（根据命中反馈动态调整）
  adaptive:
    enabled: true
    min_threshold: 0.85
    max_threshold: 0.98
    adjustment_rate: 0.01

  # 选择性缓存（跳过不适合缓存的请求）
  policy:
    enabled: true
    skip_models: ["dall-e-3"]       # 生成型模型不缓存
    max_temperature: 0.8            # 高随机性请求不缓存
    skip_streaming: false
```

### 效果预期

- 重复查询场景（客服/FAQ）：缓存命中率 40–70%，直接省 40–70% 调用成本
- 通用对话场景：缓存命中率 10–30%
- ONNX 嵌入比 Hash 嵌入的语义匹配准确率高 3–5 倍

## 第 2 层：安全护栏拦截

被安全护栏拦截的请求**不会到达上游 AI 服务**，零 Token 消耗：

```
恶意注入请求 ──► InjectionDetector ──► 403 Rejected（零成本）
```

典型场景：
- Prompt 注入攻击
- 编码绕过尝试
- 话题越界请求
- 滥用行为（短时间大量被拦截的 Key 自动降级）

## 第 3 层：智能路由

### 路由策略对比

```
策略           │ 选择方式                │ 适用场景
───────────────┼─────────────────────────┼──────────────────
basic          │ 显式模型 → tag → 默认   │ 固定模型，不需优化
cost_aware     │ 按请求字符数分三档       │ 简单场景，基础省钱
ml             │ 成本×质量×延迟三维评分   │ 多模型混用，动态优化
```

### MLRouter 工作流程

```
请求到达
    │
    ▼
┌──────────────────┐
│ 显式模型？        │──── 是 ──► 直接使用该模型
└────────┬─────────┘
         │ 否
         ▼
┌──────────────────┐
│ quality_tier     │
│ 过滤？           │
│                  │
│ economy: 取便宜半│
│ premium: 取贵半  │
└────────┬─────────┘
         │
         ▼
┌──────────────────────────────────────┐
│ 对每个候选模型计算综合评分            │
│                                      │
│  score = 0.4 × CostScore            │
│        + 0.35 × QualityScore         │
│        + 0.25 × LatencyScore         │
│                                      │
│  CostScore = 归一化(最便宜=1, 最贵=0)│
│  QualityScore = 历史成功率(EMA更新)   │
│  LatencyScore = 归一化(最快=1, 最慢=0)│
└────────┬─────────────────────────────┘
         │
         ▼
┌──────────────────┐
│ 选择最高分模型    │
└────────┬─────────┘
         │
         ▼
    调用模型
         │
         ▼
┌──────────────────┐
│ reportOutcome    │  反馈延迟和成功/失败
│ 更新 EMA 统计    │  下次路由更准确
└──────────────────┘
```

### MLRouter 配置

```yaml
routing:
  type: ml
  ml:
    cost_weight: 0.4       # 成本权重（值越大越省钱）
    quality_weight: 0.35   # 质量权重（值越大越注重成功率）
    latency_weight: 0.25   # 延迟权重（值越大越注重响应速度）
```

**省钱优先**配置建议：
```yaml
routing:
  type: ml
  ml:
    cost_weight: 0.6       # 提高成本权重
    quality_weight: 0.25   # 降低质量要求
    latency_weight: 0.15   # 降低延迟要求
```

### A/B 测试路由

A/B 测试用于对比不同模型的性价比，找到最优组合：

```
┌──────────────────────────────────────────────────────┐
│            A/B 测试实验：gpt4o-vs-claude             │
│                                                      │
│  request_id  ──► hash(exp + id) % 100               │
│                                                      │
│  ┌─── 0~49 (50%) ──► gpt-4o ────────┐              │
│  │                                    │              │
│  │    比较: 成本、质量评分、延迟       │              │
│  │                                    │              │
│  └─── 50~99 (50%) ──► claude-sonnet ─┘              │
│                                                      │
│  结果写入 ctx.ab_experiment / ctx.ab_variant         │
│  通过 CostTracker + QualityScorer 数据对比两组效果   │
└──────────────────────────────────────────────────────┘
```

配置示例：
```yaml
routing:
  type: cost_aware            # 基础路由策略
  ab_tests:
    - name: "gpt4o-vs-claude"
      variants:
        - model: "gpt-4o"
          weight: 50
        - model: "claude-3-sonnet"
          weight: 50
      enabled: true
      tenant_id: ""           # 空 = 全局生效
    - name: "mini-vs-qwen"
      variants:
        - model: "gpt-4o-mini"
          weight: 70
        - model: "qwen-turbo"
          weight: 30
      enabled: true
      tenant_id: "tenant-dev" # 仅对 tenant-dev 生效
```

## 第 4 层：租户成本限额

```
请求到达
    │
    ▼
┌────────────────────────┐
│ 查询租户当日已消费      │
│ getTenantCostInPeriod  │
└────────┬───────────────┘
         │
    已消费 ≥ 日限额?
    ┌────┴────┐
   YES       NO
    │         │
    ▼         ▼
429 拒绝   继续处理
"Daily cost    │
 limit        同理检查月限额
 exceeded"
```

配置方式（通过 Admin API 或 CLI）：
```bash
./aegisctl tenant create \
  --name "team-dev" \
  --daily-cost-limit 50.0 \
  --monthly-cost-limit 1000.0
```

## 第 5 层：用量预测

### 预测算法

```
历史数据（日聚合成本）
    │
    ▼
┌──────────────────────┐
│  OLS 线性回归        │
│                      │
│  cost = slope × day  │
│       + intercept    │
│                      │
│  R² 评估拟合优度     │
└────────┬─────────────┘
         │
    ┌────┴────┐
    │         │
    ▼         ▼
 趋势预测   预算耗尽
 未来N天    估算日期
 成本走势
```

### API 使用

**用量趋势预测：**
```bash
curl "http://localhost:8080/admin/api/predict/usage?\
tenant_id=team-dev&history_days=30&forecast_days=7" \
  -H "Cookie: aegis_session=..."
```

返回：
```json
{
  "daily_trend": 12.5,
  "r_squared": 0.87,
  "historical": [
    {"date": "2026-03-20", "total_cost": 45.2, "request_count": 1520},
    {"date": "2026-03-21", "total_cost": 58.7, "request_count": 1890}
  ],
  "predicted": [
    {"date": "2026-03-27", "total_cost": 71.2, "request_count": 0},
    {"date": "2026-03-28", "total_cost": 83.7, "request_count": 0}
  ]
}
```

**预算耗尽估算：**
```bash
curl "http://localhost:8080/admin/api/predict/budget?\
tenant_id=team-dev&budget=1000&history_days=30" \
  -H "Cookie: aegis_session=..."
```

返回：
```json
{
  "daily_trend": 12.5,
  "r_squared": 0.87,
  "budget": 1000.0,
  "budget_exhaustion_date": "2026-04-15",
  "historical": [...],
  "predicted": [...]
}
```

## 配置说明（Token 路径）

除缓存 / 路由 / 预算外，网关还可通过顶层 `token_optimization` YAML 块压缩 prompt 与输出（见 `config/aegisgate.yaml`）：

```yaml
token_optimization:
  prompt_compression:
    enabled: true
    max_context_messages: 20
    compress_whitespace: true
    dedup_system_prompts: true
  smart_max_tokens:
    enabled: true
    default_max_output: 2048
    max_output_ratio: 2.0
    min_output_tokens: 100
```

| 配置键 | 作用 |
|--------|------|
| `token_optimization.prompt_compression.*` | 限制历史消息长度，可选空白压缩 / system 去重 |
| `token_optimization.smart_max_tokens.*` | 推导有界 `max_tokens`，避免过大固定默认值 |

这些键位于顶层 `token_optimization.*` 根下（见 `config/aegisgate.yaml`）。超限拒绝或降级见 `budget_guard.*`（第 4 层）。

## 成本优化最佳实践

### 1. 启用语义缓存 + ONNX 嵌入

ONNX 嵌入能识别语义相似的不同表述（如"Python是什么？"和"介绍一下Python"），Hash 嵌入只能匹配字面相同的请求。

### 2. 使用 MLRouter 而非固定模型

让系统根据实际表现动态选择模型，避免全部请求都用最贵的模型。

### 3. 先用 A/B 测试再决定默认模型

在正式切换模型前，用 A/B 测试量化对比两个模型的成本和质量差异。

### 4. 设置合理的成本限额

为每个租户设置日/月限额，防止意外的费用暴增。

### 5. 定期查看预测报告

通过 `/admin/api/predict/budget` 接口提前发现预算不足的趋势。

### 6. 优化缓存策略

- 高 temperature（>0.8）的请求关闭缓存
- 生成型任务（如 DALL-E）跳过缓存
- 客服/FAQ 场景提高缓存 TTL

## 相关文档

- [架构指南](./architecture_zh.md) — 系统全景与组件交互
- [管理 API 参考](./admin-api_zh.md) — 预测接口详细参数
- [性能调优](./performance-tuning_zh.md) — 缓存与限流配置
