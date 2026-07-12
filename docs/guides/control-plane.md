# AegisGate Control Plane Guide

> Feature: Versioned ConfigBundle + W3 dual-approval review (Phase 9.3)
> Available: v1.1+ (optional; build with `-DENABLE_CONTROL_PLANE=ON`)
> Binaries: `aegisgate-control-plane` (server) + `aegisctl config …` (client)
> gRPC contract: `api/control-plane/proto/control_plane/v1/control_plane.proto`
> OpenAPI view: [`control-plane-v1.yaml`](../../api/control-plane/openapi/control-plane-v1.yaml)

The AegisGate **control plane** decouples *“who is allowed to change the
gateway’s YAML”* from *“when the change actually goes live.”* Every update
is submitted, reviewed by a second operator (SR5 enforces *submitter ≠
reviewer*), activated with an explicit command, and hashed into the audit
chain. Rollback to a previously-active version is a one-command operation
that does **not** re-run the approval workflow (R2 exemption).

This guide explains how to enable the control plane, bootstrap users, drive
a full apply → approve → activate cycle, and harden the deployment with
mTLS and per-user rate limits.

A Chinese translation is available at
[`control-plane_zh.md`](./control-plane_zh.md).

## Why a control plane?

Plain-file config (`config/aegisgate.yaml`) is fine while a handful of
operators share a host. Once the gateway ships into regulated environments
you typically want, simultaneously:

1. **Versioning** — every YAML ever served, with a ULID identifier,
   SHA-256, and an immutable audit chain hash.
2. **Dual approval** — the operator who writes the change cannot be the
   one who ships it (SR5).
3. **Atomic, signalable activation** — the data plane (`aegisgate`)
   reloads without a restart or file-watcher race.
4. **Historical rollback** — a one-command return to any previously-ACTIVE
   version without round-tripping a new review.

`aegisgate-control-plane` + `aegisctl config` gives you all four over a
single gRPC (+ optional mTLS) surface.

## Control plane vs data plane

| | Control plane (`aegisgate-control-plane`) | Data plane (`aegisgate`) |
|---|---|---|
| Transport | gRPC on `:9443` (default) | HTTP/HTTPS on `:8080` / `:8443` |
| Persistence | Postgres / SQLite for `config_versions`, users, api_keys, audit | In-memory cached config, reloadable via SIGHUP |
| RBAC | `SuperAdmin`-only (SR1) | Tenant-scoped (users, routes) |
| Restartable | Yes — stateless apart from the DB | Not required — SIGHUP reloads |
| Ships by default | **No** — opt-in via `ENABLE_CONTROL_PLANE=ON` | Yes |

The control plane has no HTTP surface, no request pipeline, and no routing —
it is purely a gRPC façade in front of `ConfigServiceCore` + `AuditLogger`.

## Build / install

The control plane is **off by default**. Operators who never enable it pay
zero binary-size or link-time cost.

```bash
# enable the feature + link dependencies (grpc++, protobuf, sqlite/libpq)
cmake -B build-cp-on \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DENABLE_CONTROL_PLANE=ON \
      -DVCPKG_MANIFEST_FEATURES='control-plane'
cmake --build build-cp-on -j"$(nproc)" \
      --target aegisgate aegisgate-control-plane aegisctl
```

vcpkg ships the control-plane feature in `vcpkg.json`; no manual
`vcpkg install` step is required.

## Quick start (local laptop)

The fastest way to see the full apply → approve → activate flow is the
bundled smoke test:

```bash
# assumes you already built build-cp-on/ (see above)
bash scripts/test-control-plane-local.sh
```

That script:

1. spins up `aegisgate-control-plane` on `127.0.0.1:19443` with dev TLS,
2. seeds two SuperAdmin users (`alice`, `bob`),
3. drives `config apply` (alice) → `config approve` (bob) →
   `config activate` (alice),
4. verifies `aegisctl config current` reports the expected `version_id`,
5. verifies `sha256(data-plane yaml) == sha256(aegisctl config show <vid>)`.

It tears everything down on exit (or Ctrl-C / SIGTERM).

See [`scripts/test-control-plane-local.sh`](../../scripts/test-control-plane-local.sh)
for the exact sequence — it doubles as executable documentation.

## Server configuration (`aegisgate-control-plane.yaml`)

```yaml
edition: enterprise                 # RBAC is required (SR1)
storage:
  persistent_backend: postgres      # or sqlite for single-node ops
  postgres:
    connection_string: >
      host=db user=aegisgate password=${PGPASSWORD}
      dbname=aegisgate_cp sslmode=verify-full
      sslrootcert=/etc/aegisgate/ca.crt

tls:                                # SR7 fail-closed: no knob disables this
  cert_path: /etc/aegisgate/certs/server.crt
  key_path:  /etc/aegisgate/certs/server.key
  mutual: true                      # require client certs
  client_ca_path: /etc/aegisgate/certs/client_ca.crt
  allowed_client_fingerprints_sha256:
    - b3f1…                         # from `gen-control-plane-dev-certs.sh`

control_plane:
  server:
    listen_address: 0.0.0.0:9443
    grpc_max_recv_msg_size_bytes: 1048576   # matches SR2 1 MiB cap
  submit_rate_limit_per_user_per_min: 10   # SR10
  max_yaml_size_bytes: 1048576              # SR2
  bootstrap_from_active_yaml: /etc/aegisgate/aegisgate.yaml   # optional (Q5)
```

Run it:

```bash
aegisgate-control-plane --config /etc/aegisgate/aegisgate-control-plane.yaml
```

Signals:

| Signal          | Effect                                     |
|-----------------|--------------------------------------------|
| `SIGINT`/`SIGTERM` | graceful shutdown with 5-second drain  |

There is **no** flag to disable TLS on the control-plane server — SR7
requires it to fail closed. Use `mutual: false` if you cannot issue client
certs, but the transport itself is always encrypted.

## Client configuration (`aegisctl`)

`aegisctl` reads global flags from argv and falls back to environment
variables:

| Flag / env                                  | Purpose |
|---------------------------------------------|---------|
| `--endpoint` / `AEGISGATE_CP_ENDPOINT`      | `host:port` of the server (default `localhost:9443`) |
| `--tls-ca` / `AEGISGATE_CP_TLS_CA`          | CA bundle used to verify the server cert |
| `--tls-cert` / `AEGISGATE_CP_TLS_CERT`      | Client cert (mTLS) |
| `--tls-key` / `AEGISGATE_CP_TLS_KEY`        | Client key (mTLS) |
| `--insecure-plaintext`                      | **Dev only.** Skip TLS entirely. Rejected in production builds. |
| `--output {table,json}`                     | Output format for the read commands |
| `AEGISGATE_CP_API_KEY`                      | **Only** way to pass the Bearer API Key (SR8). Never use CLI args. |

SR8 rationale: command-line arguments show up in `ps`, in shell history,
and in process-monitoring tools; environment variables do not, unless the
user explicitly leaks them.

## W3 dual-approval workflow

```text
PENDING  -- approve (reviewer ≠ submitter) --> APPROVED
PENDING  -- reject  (reviewer ≠ submitter) --> REJECTED      (terminal)
APPROVED -- reject                         --> REJECTED      (terminal)
APPROVED -- activate                       --> ACTIVE   (prev ACTIVE -> SUPERSEDED)
ACTIVE   -- rollback-to (older ACTIVE)     --> remains ACTIVE (idempotent)
SUPERSEDED -- rollback-to                  --> ACTIVE        (R2 exemption)
```

Each transition writes an audit entry with a chained SHA-256 hash
(`chain_hash`), tamper-evident across the full series.

### End-to-end example

```bash
export AEGISGATE_CP_ENDPOINT=cp.aegisgate.internal:9443
export AEGISGATE_CP_TLS_CA=/etc/aegisgate/certs/ca.crt

# alice submits
export AEGISGATE_CP_API_KEY="${ALICE_TOKEN}"
aegisctl config apply ./aegisgate.yaml --comment 'bump llm pool to 32 workers'
#   Submitted:
#   version_id: 01HZ2…V4W
#   status:     PENDING
#   sha256:     9e1b…
#   …

# bob reviews + approves
export AEGISGATE_CP_API_KEY="${BOB_TOKEN}"
aegisctl config diff 01HZ2…V4W            # see what changed
aegisctl config approve 01HZ2…V4W --comment 'LGTM — workers doubled for LLM surge'

# alice activates, telling the data plane where to land the YAML and
# which PID to SIGHUP
export AEGISGATE_CP_API_KEY="${ALICE_TOKEN}"
aegisctl config activate 01HZ2…V4W \
    --comment 'activate — coordinated with sre-oncall' \
    --data-plane-config-path /etc/aegisgate/aegisgate.yaml \
    --signal-pid $(pgrep -x aegisgate)
```

`config activate` performs the following, in order, with hard rollback if
any step fails:

1. gRPC `ActivateVersion` (server flips `status` atomically in DB).
2. Atomic file write: `fsync` the new yaml to a sibling tempfile, then
   `rename(2)` onto `--data-plane-config-path`.
3. `kill -HUP <signal-pid>` — the data plane reloads in place.

If step 2 or 3 fails, the CLI issues a compensating `RollbackVersion` so
the cluster state does not drift.

## mTLS setup

Development certificates (throwaway CA, unencrypted keys) can be generated
with:

```bash
scripts/gen-control-plane-dev-certs.sh /etc/aegisgate/certs/dev
# -> ca.crt, server.{crt,key}, client.{crt,key}, client.sha256
```

Add the printed `client.sha256` fingerprint to
`tls.allowed_client_fingerprints_sha256` in the server config. For
production use your corporate CA and rotate with your usual key-management
procedure; the control plane only needs `ca.crt` for verification and the
SHA-256 fingerprint list for the allow-list.

## Observability

The control plane emits the same Prometheus metrics family as the data
plane (counter `control_plane_rpc_total{rpc,status}`, histogram
`control_plane_rpc_latency_ms_bucket{rpc}`, gauge
`control_plane_audit_chain_length`). Point your Prometheus scrape config
at `:9443/metrics` and dashboard off `rpc="activate"` for change
observability.

Audit events live in the `audit_events` table with `chain_hash`
referencing the control plane’s namespace (`config_version:*`). Chain
inspection lives in the same `aegisctl audit verify` tool as the data
plane (Phase 7).

## Troubleshooting

| Symptom | Likely cause | Mitigation |
|---|---|---|
| `PERMISSION_DENIED: SuperAdmin role required` | the resolved user is not `super_admin` | escalate role via `aegisctl users set-role` or re-mint the API key against a SuperAdmin account (SR1) |
| `FAILED_PRECONDITION: submitter cannot approve own version` | same user did `apply` + `approve` | SR5 blocks self-approval. Use a different operator account. |
| `RESOURCE_EXHAUSTED: rate limit` on `config apply` | burst above 10/min/user | bump `control_plane.submit_rate_limit_per_user_per_min` or slow down (SR10) |
| `INVALID_ARGUMENT: yaml exceeds 1048576 bytes` | submitted YAML too large | SR2 cap — split the bundle or factor repeated blocks |
| `UNAUTHENTICATED: missing bearer token` | `AEGISGATE_CP_API_KEY` not set | SR8: token MUST be in the env, never on the command line |
| data plane never reloads after `activate` | wrong `--signal-pid`, or data-plane user can’t be signalled | check `pgrep aegisgate`, and make sure the control-plane CLI runs as a user allowed to `kill` the data plane |
| `activate` reports success but the data plane serves the old YAML | the target path is not what the data plane actually reads | confirm with `ps -fp $(pgrep aegisgate)` that argv matches `--data-plane-config-path` |

## Operational SLOs

| SLO | Target | Rationale |
|---|---|---|
| `config apply` p95 latency | < 150 ms (with 1 MiB yaml) | validation + RE2 SR4 scan + hash |
| `config activate` p95 latency | < 100 ms (DB transaction + fsync) | SR7/R2 demand consistency first |
| Control-plane reboot time | < 3 s cold-start | stateless apart from DB |
| Audit chain verification | O(n), throughput 5k rows/s | SHA-256 only |

## Roadmap

- **Phase 9.3.3** — Argo CD reference deployment; `aegisctl config apply`
  becomes an Argo CD PostSync hook.
- **Phase 9.4** — Kubernetes Operator (`aegisgateconfigs.aegisgate.io`),
  reconciling CRs against the control plane.
- **Phase 9.5** — Multi-tenant surfaces with per-tenant reviewers.

See [`docs/ROADMAP.md`](../ROADMAP.md) for the full plan.

## Reference material

- Proto contract: [`api/control-plane/proto/control_plane/v1/control_plane.proto`](../../api/control-plane/proto/control_plane/v1/control_plane.proto)
- OpenAPI view: [`control-plane-v1.yaml`](../../api/control-plane/openapi/control-plane-v1.yaml)
- Smoke test: [`scripts/test-control-plane-local.sh`](../../scripts/test-control-plane-local.sh)
- Dev certs: [`scripts/gen-control-plane-dev-certs.sh`](../../scripts/gen-control-plane-dev-certs.sh)
- Sync guard: [`scripts/verify-openapi-sync.sh`](../../scripts/verify-openapi-sync.sh)
