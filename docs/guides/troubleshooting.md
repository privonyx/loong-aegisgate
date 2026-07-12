# AegisGate Troubleshooting & FAQ

This guide summarizes common problems and a sensible troubleshooting order. Default gateway URL `http://127.0.0.1:8080`, configuration file `config/aegisgate.yaml`, model configuration `config/models.yaml`.

## Quick checklist

1. Is the process listening: `curl -s http://127.0.0.1:8080/health`
2. Is configuration valid: `aegisctl config validate config/aegisgate.yaml`
3. Are environment variables set: `AEGISGATE_API_KEY`, upstream `*_API_KEY` values
4. Error codes: cross-check HTTP status and `AEGIS-xxxx` in the [error codes reference](./error-codes.md)

## Gateway won't start or can't connect

| Symptom | Troubleshooting |
|---------|-----------------|
| Port in use | Change `server.port` or stop the process using the port |
| Configuration parse failed | Run `config validate`; check YAML indentation and quoting |
| Missing library (ONNX) | Confirm `LD_LIBRARY_PATH` or turn off the ONNX build and use hash embeddings |
| `health` shows `initialized: false` | Review startup logs; confirm the config path and that `models_config` is readable |

```bash
./build/aegisctl health
./build/aegisctl config validate config/aegisgate.yaml --strict
```

## Common error codes and handling

| Code | Brief remediation |
|------|-------------------|
| AEGIS-1001 | Check `Authorization: Bearer` against `auth.api_keys` / environment variables |
| AEGIS-1003 | Admin APIs use `auth.admin_key`; do not use a regular API key |
| AEGIS-2001 | Lower concurrency or raise `rate_limit`; distinguish upstream 429 |
| AEGIS-2003 | Check for false positives on abuse detection; tune `security.abuse_detection` |
| AEGIS-3001～3005 | See “Security false positives” below |
| AEGIS-4003 | Increase `timeout_ms`; check network and upstream health |
| AEGIS-4004 | Inspect upstream API error bodies; verify key and `base_url` |
| AEGIS-5002 | Shrink the request body or raise `limits.max_request_body_size` |
| AEGIS-9003 | Semantic cache unavailable; check embedding model and cache settings |

## Upstream timeouts and 502/504

1. In `models.yaml`, increase `timeout_ms` for the relevant provider.  
2. Use traceroute / curl directly to the upstream to rule out firewall and DNS issues.  
3. Enable a fallback chain so a single model failure does not immediately yield **AEGIS-4004**.  
4. Inspect upstream-related fields in audit logs (mind redaction).

## Performance: slow responses

- **Cache misses**: Tune `cache.threshold`; check whether streaming requests are skipped by policy (`cache.policy`).  
- **Slow embeddings**: ONNX first load, CPU contention; consider a separate deployment or lower concurrency.  
- **Too many guardrail rules**: Trim rules under `config/rules`, enable per environment.  
- **Too many threads**: Try pinning `server.threads` near CPU core count and compare with `aegisctl bench`.

```bash
./build/aegisctl --api-key "$AEGISGATE_API_KEY" \
  bench --concurrency 10 --requests 100 --model your-model
```

## Performance: high memory usage

- Lower `cache.max_entries`, `cache.max_partitions`.  
- Shorten `cache.ttl_seconds`.  
- Check whether many long-context entries are sitting in cache.

## Security false positives (guardrails too sensitive)

1. **Injection (3001)**: Edit `config/rules/injection_patterns.yaml`, remove overly broad patterns; reproduce in staging.  
2. **PII (3002)**: Adjust `pii_patterns.yaml`; desensitize on the business side before calling the gateway.  
3. **Topic (3003)**: Relax `topic_whitelist` or tighten user prompts.  
4. **Encoding (3005)**: Tune `security.encoding_min_base64_length` and related settings.  

After each change, use `admin/reload` or restart, and watch the share of 403s in metrics.

## Cache: low hit rate

- Lower `threshold` (watch for false hits).  
- Confirm the requested model is not excluded by `cache.policy.skip_models`.  
- High temperature or streaming skipped by policy is expected.  
- Switching between ONNX and hash embeddings changes the vector space; old entries may become invalid.

## Cache: results “stale” or wrong

- Check `ttl_seconds`; shorten TTL for critical workloads.  
- Same question phrased differently and low similarity is an inherent limitation of semantic cache.

## Logs and audit

**Live audit stream (SSE)**:

```bash
./build/aegisctl --url http://127.0.0.1:8080 \
  --api-key "$AEGISGATE_ADMIN_KEY" \
  logs tail --level warn
```

Note: `--api-key` here must be the **admin** key. Optional `--level` filtering.

**Application logs**: Raise `logging.level` (e.g. `debug`) for short-term diagnosis; do not leave this on in production long term.

## Config validation commands

```bash
./build/aegisctl config validate config/aegisgate.yaml
./build/aegisctl config validate config/aegisgate.yaml --strict
```

Run validation in CI so broken YAML never ships.

## When still stuck

- Collect: `health` output, relevant error JSON, snippets of `aegisgate` startup logs (redacted).  
- Cross-check request shape against `docs/openapi.yaml`.  
- When opening an issue, include version, build flags (e.g. `ENABLE_ONNX`), and a minimal repro.

## SSE / streaming disconnects

If the client drops mid-stream:

- The gateway and upstream may still consume tokens; that is expected—the client should `abort` cleanly and avoid reconnect storms.  
- If half-reads or proxy timeouts are frequent, check Nginx `proxy_read_timeout`, load balancer idle timeouts, etc.

## Common `aegisctl` issues

| Symptom | What to do |
|---------|------------|
| `logs tail` returns 401 | Use the admin key, not a regular API key |
| `bench` high error rate | Ensure `--model` matches an id in `models.yaml`; check upstream availability |
| `cache import` fails | Validate JSON and admin permissions; read `AEGIS` codes in the response body |

Environment variables `AEGISGATE_URL`, `AEGISGATE_API_KEY` shorten the CLI, but do not confuse them with “use admin key for admin operations.”

## Model list empty or not as expected

- `GET /v1/models` reflects the currently loaded `models.yaml`; reload config or restart after edits.
- If a provider’s `api_keys` env vars are unset, that provider may fail to initialize; trust startup logs.

## Redis / Postgres (optional builds)

If you build with `-DENABLE_REDIS=ON` or `-DENABLE_PG=ON`:

- Wrong connection strings or pool sizes can cause runtime degradation or errors; use `config validate` and minimal connectivity checks (`redis-cli`, `psql`).
- Under network partition you may see more cache misses rather than a crash—correlate with metrics.

## Disk and logs filling up

- `audit.log_path` and `logging.file` (if set) can grow until the disk is full, causing write failures or odd process behavior.
- Set `audit.retention_days` (non-zero when implemented for rotation/cleanup) or external logrotate; monitor disk usage.

## Crash dumps (process crashed / disappeared)

If the `aegisgate` process dies on a fatal signal (`SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL`/`SIGBUS`) or an uncaught C++ exception, it writes an in-process **crash dump log** before exiting — no extra configuration required (always on).

- **Location**: one file per crash at `logs/crash-<pid>-<epoch>.log` (relative to the run directory; tracks `logging.file`'s directory, default `logs/`). The same report is also written to `stderr`.
- **Fields**: `version`, `reason` (e.g. `signal: SIGSEGV (11)` or `terminate: uncaught exception (<type>): <what>`), `time` (epoch seconds), `pid`, and a `backtrace:`.
- **Readability**: the `aegisgate` binary is linked with `-rdynamic` (CMake `ENABLE_EXPORTS`) so the backtrace shows function names. Exact source line numbers require a core dump + `addr2line`/`gdb` (below).

```bash
ls -t logs/crash-*.log | head    # most recent crash first
cat logs/crash-<pid>-<epoch>.log
```

> Treat crash logs like internal logs: a `terminate` report includes the exception `what()` text, which may carry business context. Keep them under the same access controls as other logs.

## OS core dumps (line-level analysis)

The in-process backtrace tells you *where*; a core dump lets you inspect *state* (variables, full stacks) under `gdb`.

1. Allow core dumps for the session/service:

```bash
ulimit -c unlimited          # current shell; for systemd use LimitCORE=infinity
cat /proc/sys/kernel/core_pattern   # where cores go (often piped to systemd-coredump)
```

2. Reproduce the crash, then locate the core:

```bash
# systemd-coredump systems:
coredumpctl list aegisgate
coredumpctl gdb aegisgate          # opens gdb on the latest core

# plain core file (core_pattern is a path like core.%p):
gdb ./build/src/aegisgate core.<pid>
```

3. In `gdb`: `bt full` for a full backtrace, `info threads` / `thread apply all bt` for all threads. To map a raw address from the crash log to a source line:

```bash
addr2line -e ./build/src/aegisgate -f -C 0x<address>
```

> Build with debug info (`-DCMAKE_BUILD_TYPE=RelWithDebInfo` or `Debug`) for the most useful core-dump analysis; stripped Release binaries yield addresses but fewer symbols.

## Behavior changes after version upgrades

After a major upgrade or vcpkg baseline change:

1. Run unit tests and key integration tests.  
2. Re-run `config validate --strict`.  
3. Compare `/metrics` and sampled request latency.

## Related documents

- [Error codes reference](./error-codes.md)
- [Performance tuning](./performance-tuning.md)
- [Security best practices](./security-best-practices.md)
- [Quick start](./quick-start.md)
