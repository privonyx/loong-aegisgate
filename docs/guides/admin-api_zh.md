# AegisGate 管理 API 参考

本文档描述 AegisGate 企业版 Web 管理面板的 REST API。所有端点需要通过 JWT Cookie 认证。

## 认证

### 登录

```
POST /admin/api/auth/login
Content-Type: application/json

{"api_key": "sk_..."}
```

> **Breaking (TASK-20260508-01)**：原 `POST /admin/login` 已迁至 `/admin/api/*` 命名空间，`/admin/login` 让位给 SPA 客户端登录页。

成功后返回 `Set-Cookie: aegis_session=<jwt>` 用于后续请求认证。

### 登出

```
POST /admin/api/auth/logout
Cookie: aegis_session=<jwt>
```

> **Breaking (TASK-20260508-01)**：原 `POST /admin/logout` 已迁至 `/admin/api/auth/logout`。

### 当前用户

```
GET /admin/api/me
Cookie: aegis_session=<jwt>
```

## 租户管理

### 创建租户

```
POST /admin/api/tenants
Content-Type: application/json

{
  "name": "team-dev",
  "daily_cost_limit": 50.0,
  "monthly_cost_limit": 1000.0,
  "rate_limit_tokens": 100,
  "rate_limit_refill": 10.0,
  "model_whitelist": ["gpt-4o-mini", "deepseek-chat"]
}
```

### 列出租户

```
GET /admin/api/tenants?limit=100&offset=0
```

### 获取/更新/删除租户

```
GET    /admin/api/tenants/{id}
PUT    /admin/api/tenants/{id}
DELETE /admin/api/tenants/{id}
```

## 用户管理

```
POST   /admin/api/users              # 创建用户
GET    /admin/api/users              # 列出用户
GET    /admin/api/users/{id}         # 获取用户
PUT    /admin/api/users/{id}         # 更新用户
DELETE /admin/api/users/{id}         # 删除用户
```

角色类型：`viewer` | `developer` | `tenant_admin` | `super_admin`

## API Key 管理

```
POST /admin/api/keys               # 创建 API Key
GET  /admin/api/keys               # 列出 API Key
POST /admin/api/keys/{id}/revoke   # 吊销
POST /admin/api/keys/{id}/rotate   # 轮转
```

## 审计与成本查询

### 查询审计日志

```
GET /admin/api/audits?tenant_id=...&limit=100&offset=0
```

### 查询成本记录

```
GET /admin/api/costs?tenant_id=...&model=...&limit=100&offset=0
```

### 仪表盘概览

```
GET /admin/api/dashboard/summary
```

## 用量预测

### 用量趋势预测

```
GET /admin/api/predict/usage
```

**参数：**

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `tenant_id` | string | — | 租户 ID（SuperAdmin 可查任意租户，TenantAdmin 查自己） |
| `history_days` | int | 30 | 历史数据天数（用于拟合） |
| `forecast_days` | int | 7 | 预测天数 |

**响应：**

```json
{
  "daily_trend": 12.5,
  "r_squared": 0.87,
  "historical": [
    {
      "date": "2026-03-20",
      "total_cost": 45.2,
      "request_count": 1520
    }
  ],
  "predicted": [
    {
      "date": "2026-03-27",
      "total_cost": 71.2,
      "request_count": 0
    }
  ]
}
```

**字段说明：**

| 字段 | 说明 |
|------|------|
| `daily_trend` | 日均成本变化斜率（正数=上升趋势） |
| `r_squared` | 拟合优度（0~1，越接近 1 预测越可靠） |
| `historical` | 历史日聚合数据 |
| `predicted` | 预测数据（`request_count` 为预估值） |

### 预算耗尽估算

```
GET /admin/api/predict/budget
```

**参数：**

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `tenant_id` | string | — | 租户 ID |
| `budget` | double | 0 | 总预算金额 |
| `history_days` | int | 30 | 历史数据天数 |

**响应：**

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

**字段说明：**

| 字段 | 说明 |
|------|------|
| `budget_exhaustion_date` | 预算耗尽日期，空字符串表示预算充足（365 天内不会耗尽） |

### 预测时序图

```
Client          AdminHttpController     AdminController      UsagePredictor    PersistentStore
  │                    │                      │                    │                  │
  │── GET /predict ───►│                      │                    │                  │
  │                    │── authenticate ──────►│                    │                  │
  │                    │── predictUsage ──────►│                    │                  │
  │                    │                      │── predict ─────────►│                  │
  │                    │                      │                    │── queryByRange ──►│
  │                    │                      │                    │◄── CostRecords ──│
  │                    │                      │                    │── aggregateDaily─│
  │                    │                      │                    │── fitLinear ─────│
  │                    │                      │                    │── forecast ──────│
  │                    │                      │◄── Prediction ─────│                  │
  │                    │◄── JSON ─────────────│                    │                  │
  │◄── Response ──────│                      │                    │                  │
```

## 合规报告

### 导出审计报告

```
GET /admin/api/reports/audit?from=2026-03-01&to=2026-03-31&tenant_id=...&format=csv
```

### 导出成本报告

```
GET /admin/api/reports/cost?from=2026-03-01&to=2026-03-31&tenant_id=...&format=csv
```

## SSO 管理

### SSO 登录流程

```
GET  /admin/auth/sso/login?tenant_id=...   # 发起 OIDC 登录
GET  /admin/auth/sso/callback              # OIDC 回调
POST /admin/auth/sso/logout                # SSO 登出
```

### SSO Provider CRUD

```
POST   /admin/api/sso/providers
GET    /admin/api/sso/providers
GET    /admin/api/sso/providers/{id}
PUT    /admin/api/sso/providers/{id}
DELETE /admin/api/sso/providers/{id}
```

## MFA 管理

```
POST /admin/api/mfa/setup       # 初始化 TOTP
POST /admin/api/mfa/verify      # 验证 TOTP 码
POST /admin/api/mfa/disable     # 禁用 MFA
POST /admin/api/mfa/recovery    # 恢复码验证
```

## Prompt 模板管理

```
POST   /admin/api/templates
GET    /admin/api/templates
GET    /admin/api/templates/{id}
PUT    /admin/api/templates/{id}
DELETE /admin/api/templates/{id}
```

## 规则集管理

```
POST /admin/api/rules              # 创建规则版本
GET  /admin/api/rules              # 列出规则版本
GET  /admin/api/rules/active       # 获取当前生效规则
POST /admin/api/rules/activate     # 激活指定版本
```

## WebSocket 实时推送

```
ws://localhost:8080/admin/ws
```

连接后自动接收实时指标推送（每 2 秒）和审计事件。

消息格式：
```json
{
  "type": "metrics",
  "data": {
    "requests_total": 1520,
    "cache_hit_rate": 0.35,
    "active_connections": 12,
    "cost_today": 45.2
  }
}
```

## 错误响应格式

所有错误统一使用以下格式：

```json
{
  "error": {
    "code": "AEGIS-1001",
    "type": "authentication_error",
    "message": "Invalid API key"
  }
}
```

详见 [错误码参考](./error-codes_zh.md)。

## 相关文档

- [架构指南](./architecture_zh.md) — 系统全景
- [成本优化指南](./cost-optimization_zh.md) — 省钱策略详解
- [快速开始](./quick-start_zh.md) — 编译、配置、首次调用
