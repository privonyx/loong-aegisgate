# AegisGate Security Best Practices

When the gateway is exposed on the public internet or on a critical internal path, harden **authentication**, **transport**, **guardrail rules**, and the **operations surface** together. The following aligns with the default configuration in `config/aegisgate.yaml` and `config/rules/*.yaml`.

## API Key Management

- **Do not commit secrets to Git**: use `${ENV}` placeholders and inject `AEGISGATE_API_KEY`, `OPENAI_API_KEY`, and similar in the deployment environment.
- **Separate gateway keys from upstream keys**: `auth.api_keys` is only for clients calling the gateway; real model credentials belong in the provider section of `config/models.yaml`.
- **Rotation**: add a new key → migrate clients gradually → remove the old key from the list. Avoid long-lived exposure from a single leak.
- **Least privilege**: if you later split capabilities across multiple keys, do so by business line to simplify revocation and auditing.

## Admin Key

- `auth.admin_key` also supports reading from the environment via `${AEGISGATE_ADMIN_KEY}`.
- **Not the same as business API keys**: when `aegisctl` calls `/admin/reload`, `/admin/logs/stream`, or `/admin/cache/import`, use the admin key (the value passed with `--api-key` is the Bearer token and should be the admin key here).
- In production, use a **high-entropy random** value stored only in a secrets manager or orchestration Secret.

## Guardrail Configuration

### Injection Detection

File: `config/rules/injection_patterns.yaml`

- Review rules periodically to avoid overly broad patterns that cause false positives (see [Troubleshooting](./troubleshooting.md)).
- Use together with heuristic toggles under the `security` section; watch logs for **AEGIS-3001**.

### PII

File: `config/rules/pii_patterns.yaml`

- RE2 is used by default; mind rule complexity and maintenance cost.
- For compliance-sensitive cases: prefer blocking plus log alerts over sending plaintext PII to third-party models.

### Topic Boundaries

File: `config/rules/topic_whitelist.yaml` (and enterprise custom rules)

- Align allow/deny lists with your domain; before changes, validate **AEGIS-3003** trigger rates in a staging environment.

### Encoding and Unicode

In `aegisgate.yaml`:

```yaml
security:
  unicode_normalization: true
  encoding_detection: true
  encoding_min_base64_length: 20
```

- For legitimate Base64 payloads, if **AEGIS-3005** false positives occur, you may raise `encoding_min_base64_length` (assess the security tradeoff).

### Abuse Detection

`security.abuse_detection`: window, warn/throttle/ban thresholds and durations,
plus **content-similarity clustering** via 64-bit SimHash (`similarity_*` knobs).

- Prefer **stricter** settings on public entry points; internal toolchains can be looser.
- Similarity fingerprints are **node-local** (even when Redis backs rejection counts).
- Tune `similarity_hamming_threshold` (default `3`) if legitimate template traffic is over-flagged.
- Relates to **AEGIS-2003**; after changes, watch audit trails and metrics.

## TLS

```yaml
tls:
  enabled: true
  port: 0          # 0 or unset: defaults to HTTP port + 1, overridable via AEGISGATE_TLS_PORT
  cert_path: "/path/to/fullchain.pem"
  key_path: "/path/to/privkey.pem"
```

- In production, expose only HTTPS externally; renew certificates on a schedule (Let’s Encrypt or internal PKI).
- When a reverse proxy (Nginx/Caddy) terminates TLS, still ensure the hop **to the gateway** is trusted (or use mTLS, depending on architecture).

## Hot Reload of Rules

After configuration changes, reload via the admin API (requires **admin_key**) instead of restarting the process often:

```bash
curl -s -X POST http://127.0.0.1:8080/admin/reload \
  -H "Authorization: Bearer ${AEGISGATE_ADMIN_KEY}"
```

- **Suggested flow**: manage rules in Git → validate YAML in CI → deploy to servers → `reload` → spot-check.
- **Access control**: only operations roles hold admin; record actions in audit logs.

## Audit Logs

```yaml
audit:
  log_path: "logs/audit.log"
  retention_days: 0
```

- Logs may contain request summaries; restrict disk access and permissions (`chmod`, dedicated user).
- For long retention, forward to centralized logging (SIEM) with a redaction policy.
- Live tail: `aegisctl --api-key "$AEGISGATE_ADMIN_KEY" logs tail --level warn`

## Content Filtering (Output Side)

- When configuring output guardrail actions (replace, truncate, alert), be explicit about business impact of **fail closed** vs **fail open**.
- If **AEGIS-3004** fires often, distinguish model gibberish from overly strict rules.

## Observability and Alerting

- **`/metrics`**: set alert thresholds for 401/403/429/5xx.
- **Cost and quotas**: guard against abnormal bills from leaked keys (see `observe`-related settings and persistence).

## Configuration Validation

Before release, run:

```bash
./build/aegisctl config validate config/aegisgate.yaml
./build/aegisctl config validate config/aegisgate.yaml --strict
```

`--strict` treats warnings as errors, suitable for CI.

## Audit Integrity and Persistence

The SQLite backend (`storage.persistent_backend: sqlite`) is commonly used for audit and cost data. Recommendations:

- Restrict database file and backup permissions to the gateway runtime user.
- Back up `data/aegisgate.db` (or your custom path) regularly and verify restore procedures.
- If the audit chain includes integrity hashes (implementation details such as FNV-1a follow the source), pair tamper detection with **append-only** remote storage policies.

## Reverse Proxy and Real IP

If Nginx/Ingress sits in front of the gateway:

- **Trust chain**: use `X-Forwarded-For` for rate limiting or audit attribution only when the proxy is trustworthy.
- Avoid exposing admin paths `/admin/*` to the public internet; restrict sources with network policy or a separate listen address.

Exact header behavior depends on the current Drogon stack and project middleware; re-check after upgrades.

## Enterprise Edition and Feature Flags

`edition: enterprise` and the `features` section control routing, guardrails, admin UI, and more. On Community deployments, do not enable configuration that depends on enterprise-only features, or you risk startup failure or silent downgrade. License path is `license_file`; do not commit it to version control.

## Supply Chain and Dependencies

- Lock dependency versions with a vcpkg baseline to reduce supply-chain drift.
- Prefer **reproducible** production image builds (same tag → same dependency tree).
- Stay subscribed to security advisories for Drogon, yaml-cpp, RE2, and related components.

## Incident Response

If an API key is compromised:

1. Immediately revoke the affected entries from `auth.api_keys` and upstream key lists.
2. Apply configuration with `POST /admin/reload` or a rolling restart.
3. Search audit logs in the relevant time window for abnormal calls.
4. Rotate all potentially affected secrets.

## Related Documents

- [Error codes reference](./error-codes.md)
- [Troubleshooting](./troubleshooting.md)
- [Quick start](./quick-start.md)
