# AegisGate Admin API Reference

This document describes the REST API for the AegisGate Enterprise Edition web admin console. All endpoints require JWT cookie authentication.

## Authentication

### Login

```
POST /admin/api/auth/login
Content-Type: application/json

{"api_key": "sk_..."}
```

> **Breaking (TASK-20260508-01)**: previously `POST /admin/login`. Path moved into the `/admin/api/*` namespace so that `/admin/login` is reserved for the SPA client-side login page.

On success, returns `Set-Cookie: aegis_session=<jwt>` for authenticating subsequent requests.

### Logout

```
POST /admin/api/auth/logout
Cookie: aegis_session=<jwt>
```

> **Breaking (TASK-20260508-01)**: previously `POST /admin/logout`.

### Current user

```
GET /admin/api/me
Cookie: aegis_session=<jwt>
```

## Tenant management

### Create tenant

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

### List tenants

```
GET /admin/api/tenants?limit=100&offset=0
```

### Get / update / delete tenant

```
GET    /admin/api/tenants/{id}
PUT    /admin/api/tenants/{id}
DELETE /admin/api/tenants/{id}
```

## User management

```
POST   /admin/api/users              # Create user
GET    /admin/api/users              # List users
GET    /admin/api/users/{id}         # Get user
PUT    /admin/api/users/{id}         # Update user
DELETE /admin/api/users/{id}         # Delete user
```

Role types: `viewer` | `developer` | `tenant_admin` | `super_admin`

## API key management

```
POST /admin/api/keys               # Create API key
GET  /admin/api/keys               # List API keys
POST /admin/api/keys/{id}/revoke   # Revoke
POST /admin/api/keys/{id}/rotate   # Rotate
```

## Auditing and cost queries

### Query audit logs

```
GET /admin/api/audits?tenant_id=...&limit=100&offset=0
```

### Query cost records

```
GET /admin/api/costs?tenant_id=...&model=...&limit=100&offset=0
```

### Dashboard summary

```
GET /admin/api/dashboard/summary
```

## Usage prediction

### Usage trend prediction

```
GET /admin/api/predict/usage
```

**Parameters:**

| Parameter | Type | Default | Description |
|------|------|--------|------|
| `tenant_id` | string | — | Tenant ID (SuperAdmin may query any tenant; TenantAdmin queries own) |
| `history_days` | int | 30 | Number of days of historical data (used for fitting) |
| `forecast_days` | int | 7 | Number of days to forecast |

**Response:**

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

**Field descriptions:**

| Field | Description |
|------|------|
| `daily_trend` | Slope of daily average cost change (positive = upward trend) |
| `r_squared` | Goodness of fit (0–1; closer to 1 means a more reliable prediction) |
| `historical` | Historical daily aggregated data |
| `predicted` | Predicted data (`request_count` is an estimate) |

### Budget exhaustion estimate

```
GET /admin/api/predict/budget
```

**Parameters:**

| Parameter | Type | Default | Description |
|------|------|--------|------|
| `tenant_id` | string | — | Tenant ID |
| `budget` | double | 0 | Total budget amount |
| `history_days` | int | 30 | Number of days of historical data |

**Response:**

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

**Field descriptions:**

| Field | Description |
|------|------|
| `budget_exhaustion_date` | Date when the budget is exhausted; empty string means the budget is sufficient (will not be exhausted within 365 days) |

### Prediction sequence diagram

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

## Compliance reports

### Export audit report

```
GET /admin/api/reports/audit?from=2026-03-01&to=2026-03-31&tenant_id=...&format=csv
```

### Export cost report

```
GET /admin/api/reports/cost?from=2026-03-01&to=2026-03-31&tenant_id=...&format=csv
```

## SSO management

### SSO login flow

```
GET  /admin/auth/sso/login?tenant_id=...   # Start OIDC login
GET  /admin/auth/sso/callback              # OIDC callback
POST /admin/auth/sso/logout                # SSO logout
```

### SSO Provider CRUD

```
POST   /admin/api/sso/providers
GET    /admin/api/sso/providers
GET    /admin/api/sso/providers/{id}
PUT    /admin/api/sso/providers/{id}
DELETE /admin/api/sso/providers/{id}
```

## MFA management

```
POST /admin/api/mfa/setup       # Initialize TOTP
POST /admin/api/mfa/verify      # Verify TOTP code
POST /admin/api/mfa/disable     # Disable MFA
POST /admin/api/mfa/recovery    # Recovery code verification
```

## Prompt template management

```
POST   /admin/api/templates
GET    /admin/api/templates
GET    /admin/api/templates/{id}
PUT    /admin/api/templates/{id}
DELETE /admin/api/templates/{id}
```

**Data-plane preprocess (Chat Completions):** If the request has no `system` message, AegisGate selects an `is_active` template for the authenticated tenant (weighted by `weight`) and prepends it as `role=system`. Override with request header `X-AegisGate-Template: <name>`. On success the response includes `X-AegisGate-Template-Applied`. Existing client `system` messages are never overwritten. Variable interpolation (`{{var}}`) is not applied in v1.

## Ruleset management

```
POST /admin/api/rules              # Create rules version
GET  /admin/api/rules              # List rules versions
GET  /admin/api/rules/active       # Get currently active rules
POST /admin/api/rules/activate     # Activate specified version
```

## WebSocket real-time push

```
ws://localhost:8080/admin/ws
```

After connecting, you automatically receive real-time metric pushes (every 2 seconds) and audit events.

Message format:
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

## Error response format

All errors use the following format uniformly:

```json
{
  "error": {
    "code": "AEGIS-1001",
    "type": "authentication_error",
    "message": "Invalid API key"
  }
}
```

See [Error codes reference](./error-codes.md).

## Related documents

- [Architecture guide](./architecture.md) — system overview
- [Cost optimization guide](./cost-optimization.md) — detailed cost-saving strategies
- [Quick start](./quick-start.md) — build, configure, and first call
