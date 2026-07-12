# 多区域路由指南

> 覆盖功能：`GeoRouter`（Phase 9.1.1）
> 可用版本：v1.1+（在 v3.0 路线图 Phase 9 框架下渐进交付）

本指南介绍如何在 AegisGate 中启用 **区域感知路由（Geo-aware Routing）**。GeoRouter 根据请求发起方的地理位置，把候选模型限制在合规 / 低延迟的区域内，不改变任何现有路由器（Basic/CostAware/ML/ABTest）的行为，以装饰器方式叠加。

## 为什么需要区域感知？

| 场景 | 痛点 | GeoRouter 如何解决 |
|------|------|-------------------|
| 跨洋回源 | 亚太用户命中 us-east 端点，延迟增加 150-250 ms | 推断客户端区域，优先选择同区域模型 |
| GDPR 合规 | 欧盟流量可能被路由到非欧盟 Provider | `residency: strict` 强制数据驻留 |
| 多云部署 | 同一模型在多区域都有端点 | 按 `region:<name>` tag 按需命中 |

## 快速上手（3 步）

### Step 1 — 给模型打区域 tag

编辑 `config/models.yaml`，在每个模型 `tags` 中添加 `region:<name>`：

```yaml
models:
  - id: gpt-4o-us
    provider: openai-us
    tags: ["region:us-east", "high-quality"]
  - id: gpt-4o-eu
    provider: openai-eu
    tags: ["region:eu-central", "high-quality"]
  - id: qwen-multi
    provider: local
    # 同一模型可同时属于多个区域
    tags: ["region:us-east", "region:eu-central", "cheap"]
```

### Step 2 — 启用 `routing.geo`

编辑 `config/aegisgate.yaml`：

```yaml
routing:
  type: cost_aware        # 底层 router 不变
  geo:
    enabled: true
    affinity: prefer      # strict | prefer | any
    default_client_region: us-east
    header_names:
      - X-AegisGate-Region
      - X-Client-Region
    ip_region_map:        # 可选
      - cidr: 10.0.0.0/8
        region: us-east
      - cidr: 172.16.0.0/12
        region: eu-central
```

### Step 3 — 客户端指定区域（可选）

SDK / cURL 可通过下列方式显式声明区域（优先级从高到低）：

1. `X-AegisGate-Region: eu-central` header
2. `X-Client-Region: eu-central` header
3. 请求 JSON `extra.client_region = "eu-central"`
4. 请求来源 IP 命中 `ip_region_map`（由 HTTP 层注入到 `extra.client_ip`）
5. `routing.geo.default_client_region`

## 亲和策略对照表

| 策略 | 无匹配候选时 | 下层选出非合规模型时 | 典型场景 |
|------|-------------|---------------------|----------|
| `strict` | 返回空串（触发 fallback） | 强制重选首个合规候选 | 数据驻留 / 合规要求 |
| `prefer` | 使用全部候选（日志 warn） | 放行（日志 warn） | 性能优化首选 |
| `any` | 使用全部候选 | 放行 | 纯观测 |

## 驻留策略（residency）

租户或请求可声明 **硬驻留约束**，优先级高于 affinity：

```bash
curl -X POST https://api.example.com/v1/chat/completions \
  -H 'Authorization: Bearer YOUR_KEY' \
  -H 'X-AegisGate-Region: eu-central' \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "gpt-4o-eu",
    "messages": [{"role":"user","content":"hi"}],
    "extra": { "residency": "strict" }
  }'
```

`residency: strict` 含义：
- 无区域 tag 的模型（unregionalized）被自动排除
- 即使 `affinity=prefer`，下层路由挑选非合规模型时也会被强制重选
- 找不到合规候选时返回空串（业务层触发降级）

## 观测字段

GeoRouter 会向 `chat_request.extra` 写入以下字段（仅当 `enabled: true`）：

| 字段 | 含义 |
|------|------|
| `_geo_client_region` | 推断出的客户端区域 |
| `_geo_selected_region` | 最终选中模型的区域 tag；`unknown` 表示模型无 region tag |
| `_geo_allowed_models` | 过滤后的候选模型 ID 数组 |

这些字段可被下游审计 / 追踪 / 指标阶段消费，便于做区域路由的效果观测。

## 故障排查

| 现象 | 排查 |
|------|------|
| GeoRouter 不生效 | 检查 `routing.geo.enabled: true`；查看启动日志 "Router: wrapped with GeoRouter" |
| `strict` 策略全部失败 | 确认目标模型的 `tags` 含 `region:<name>`；检查 `default_client_region` |
| CIDR 不匹配 | 只支持 IPv4；IPv6 地址自动回退到下一级推断；检查 `client_ip` 是否由 HTTP 层正确注入 |
| region 不归一化 | 添加 `region_aliases`，如 `us: us-east` |

## 与现有 Router 的组合顺序

```
Base (Basic / CostAware / ML)
  └─► ABTestRouter（可选）
        └─► GeoRouter（可选，最外层）
```

GeoRouter **始终是最外层**，确保"区域合规性"作为请求路由的最终闸门。

## 后续计划

- **Phase 9.1.2** MaxMind GeoLite2 替代手写 CIDR
- **Phase 9.1.3** 跨区域缓存同步（CRDT）
- **Phase 9.1.4** 强制驻留 Stage（拒绝违规写入）
