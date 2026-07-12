# Cache Migration Guide

> Feature: Phase 6.2 `aegisctl cache dump|restore`
> Available since: v1.2 (TASK-20260513-01)
> 中文版本：[`cache-migration_zh.md`](cache-migration_zh.md) — authoritative source

`aegisctl cache` exports / imports the semantic cache offline so
operators can migrate between vector backends or warm a fresh cluster
from a production snapshot.

## Snapshot format

Self-describing single binary file:

```
"AGCM" (4)
version uint32          (current = 1)
dim     uint32
entries uint64
N × { cache_key, tenant_id, response, vector[dim], expire_at int64 }
tail_sha256 (32)
```

## Usage

### Dump

```bash
export AEGISGATE_CP_API_KEY="$(echo -n 'admin-secret' | sha256sum | awk '{print $1}')"
aegisctl cache dump --output /tmp/cache-snapshot.bin --tenant tenant-acme --max-entries 100000
```

### Restore

```bash
export AEGISGATE_CP_API_KEY="<sha256-hex>"
export AEGISGATE_CACHE_MIGRATE_TENANT_ALLOW="tenant-acme,tenant-foo"
aegisctl cache restore --input /tmp/cache-snapshot.bin --skip-expired
```

The CLI verifies the tail SHA-256 (SR2) before any write; mismatch
aborts with exit code 2.

## Security

- **SR2** — tail SHA-256 verified before restore writes anything.
- **SR8** — both subcommands require `AEGISGATE_CP_API_KEY` matching
  the control-plane admin token sha256; missing key → exit 2.
- **Tenant allowlist** — `AEGISGATE_CACHE_MIGRATE_TENANT_ALLOW` filters
  records during restore, blocking accidental cross-tenant import.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | I/O / VectorStore error |
| 2 | security check failed |
| 3 | unrecognised format |

## Verification

```bash
# Dump → restore round-trip
aegisctl cache dump --output /tmp/a.bin
sha256sum /tmp/a.bin

# Verify SR2: flip one byte then restore — must exit 2
dd if=/dev/urandom of=/tmp/a.bin conv=notrunc bs=1 count=1 seek=100
aegisctl cache restore --input /tmp/a.bin   # → "snapshot tail sha256 mismatch", exit 2
```

`tests/unit/cache/test_cache_migrator.cpp` already covers the SR2 byte-flip,
SR8 missing-key, and tenant-allowlist mutation experiments.

## References

- CLI source: `src/cli/aegisctl.cpp` (`cache dump|restore` dispatcher)
