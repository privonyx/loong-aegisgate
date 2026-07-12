#!/usr/bin/env bash
# scripts/test-control-plane-local.sh
#
# Phase 9.3 Epic 8.2 — two-process end-to-end smoke test.
#
# Flow:
#   1. mktemp -d workdir, mode 0700
#   2. generate dev TLS certs (scripts/gen-control-plane-dev-certs.sh)
#   3. start aegisgate-control-plane once to auto-create the sqlite schema,
#      shut it down, seed alice + bob SuperAdmin users + API keys
#   4. restart aegisgate-control-plane (records $CP_PID)
#   5. exercise aegisctl config apply (as alice) → approve (as bob) →
#      activate (with --data-plane-config-path + --signal-pid)
#   6. Verify A: `aegisctl config current` reports the activated version_id
#   7. Verify B: sha256(dp target yaml) == sha256(aegisctl config show <vid>)
#   8. trap EXIT: kill children, rm -rf workdir
#
# Validation strategy follows design D2 (double-check via gRPC current + file
# sha256), deliberately avoiding a dependency on any data-plane /metrics
# surface.
#
# Exit codes:
#   0  PASS
#   1  FAIL at Verify A (gRPC state mismatch)
#   2  FAIL at Verify B (data-plane yaml sha256 mismatch)
#   3  FAIL at boot / precondition (missing binary, port bind timeout, etc.)
#
# Dev hooks for tests/scripts/test_cp_integration_cleanup.sh:
#   AEGISGATE_IT_MARKER_DIR      -- if set, the script writes "tmp_path" with
#                                   the workdir path so external tests can
#                                   assert cleanup happened.
#   AEGISGATE_IT_PAUSE_AFTER_BOOT -- if set to a positive integer N, the
#                                   script sleeps N seconds after binding
#                                   both processes and BEFORE running any
#                                   aegisctl command. Used to deterministically
#                                   observe the cleanup trap.

set -euo pipefail

# ---- logging ----------------------------------------------------------------
log()  { printf '[IT %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die()  { log "FATAL: $*"; exit "${2:-3}"; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# ---- binary discovery -------------------------------------------------------
# CMake emits executables under build-cp-on/src/... by default. We search
# the top-level build dir first so operators who install the binaries into
# a flat layout (CI, distro packaging) also work.
find_binary() {
    local name="$1"; local build_root="$2"
    for candidate in \
        "$build_root/$name" \
        "$build_root/src/$name" \
        "$build_root/src/control_plane/$name"; do
        [[ -x "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
    done
    return 1
}

CP_BIN="${AEGISGATE_IT_CP_BIN:-$(find_binary aegisgate-control-plane "$REPO/build-cp-on" || true)}"
CTL_BIN="${AEGISGATE_IT_CTL_BIN:-$(find_binary aegisctl "$REPO/build-cp-on" || true)}"
DP_BIN="${AEGISGATE_IT_DP_BIN:-$(find_binary aegisgate "$REPO/build" 2>/dev/null \
                              || find_binary aegisgate "$REPO/build-cp-on" \
                              || true)}"

[[ -x "$CP_BIN"  ]] || die "missing binary: aegisgate-control-plane (build the ON-path first)"
[[ -x "$CTL_BIN" ]] || die "missing binary: aegisctl (build the ON-path first)"

# The data-plane binary is optional — Verify B degrades gracefully when it
# is missing because aegisctl can write the yaml target itself (via
# --data-plane-config-path). If DP_BIN isn't available we skip launching it
# but still run both verifies.
DP_AVAILABLE=0
if [[ -x "$DP_BIN" ]]; then
    DP_AVAILABLE=1
else
    log "note: $DP_BIN not present — Verify B still runs against the file aegisctl writes"
fi

command -v python3    >/dev/null || die "python3 not installed (needed for DB seeding)"

# ---- workdir + cleanup trap -------------------------------------------------
TMP="$(mktemp -d -t aegisgate-it.XXXXXX)"
chmod 700 "$TMP"
log "workdir: $TMP"

if [[ -n "${AEGISGATE_IT_MARKER_DIR:-}" && -d "$AEGISGATE_IT_MARKER_DIR" ]]; then
    printf '%s\n' "$TMP" > "$AEGISGATE_IT_MARKER_DIR/tmp_path"
fi

CP_PID=0
DP_PID=0

# shellcheck disable=SC2317  # invoked indirectly via `trap cleanup EXIT INT TERM`
cleanup() {
    local rc=$?
    set +e
    log "cleanup entered (exit code so far: $rc)"
    if [[ "$DP_PID" -gt 0 ]]; then
        kill -TERM "$DP_PID" 2>/dev/null
    fi
    if [[ "$CP_PID" -gt 0 ]]; then
        kill -TERM "$CP_PID" 2>/dev/null
    fi
    sleep 1
    [[ "$DP_PID" -gt 0 ]] && kill -KILL "$DP_PID" 2>/dev/null
    [[ "$CP_PID" -gt 0 ]] && kill -KILL "$CP_PID" 2>/dev/null
    rm -rf "$TMP"
    log "cleanup done"
    return "$rc"
}
trap cleanup EXIT INT TERM

# ---- certs ------------------------------------------------------------------
log "generating dev TLS certs"
bash "$REPO/scripts/gen-control-plane-dev-certs.sh" "$TMP/certs" >/dev/null

# ---- config files -----------------------------------------------------------
CP_CFG="$TMP/cp.yaml"
DP_SEED="$TMP/dp-seed.yaml"      # data-plane yaml content we'll apply
DP_TARGET="$TMP/dp-target.yaml"  # where activate writes (data-plane reads here)

# Minimal control-plane config. port 0 is not supported by our config loader,
# so we pick a high port (19443) that's unlikely to collide on CI runners.
cat > "$CP_CFG" <<EOF
edition: enterprise
storage:
  persistent_backend: sqlite
  sqlite:
    path: $TMP/cp.sqlite
    wal: true
tls:
  cert_path: $TMP/certs/server.crt
  key_path:  $TMP/certs/server.key
  mutual: false
control_plane:
  server:
    listen_address: 127.0.0.1:19443
  submit_rate_limit_per_user_per_min: 100
  max_yaml_size_bytes: 1048576
EOF

# Data-plane seed yaml we'll submit. Must itself validate per Config::validate.
cp "$REPO/config/aegisgate.yaml" "$DP_SEED"

# ---- phase 1: first boot to create schema -----------------------------------
log "phase 1: boot control-plane once to auto-create sqlite schema"
"$CP_BIN" --config "$CP_CFG" >"$TMP/cp-init.log" 2>&1 &
init_pid=$!

for _ in $(seq 1 40); do
    ss -tln 2>/dev/null | grep -q ':19443 ' && break
    sleep 0.5
done
if ! ss -tln 2>/dev/null | grep -q ':19443 '; then
    cat "$TMP/cp-init.log" >&2
    die "control-plane (init boot) failed to bind :19443"
fi

kill -TERM "$init_pid"
wait "$init_pid" 2>/dev/null || true
log "schema created; control-plane stopped"

# ---- phase 2: seed alice + bob ----------------------------------------------
ALICE_TOKEN="integration-test-alice-token-4f2c7a9b"
BOB_TOKEN="integration-test-bob-token-9e3d1c6f"

log "seeding sqlite with alice + bob SuperAdmin users + api keys"
python3 - "$TMP/cp.sqlite" "$ALICE_TOKEN" "$BOB_TOKEN" <<'PY'
import sqlite3, hashlib, sys, time
db_path, alice, bob = sys.argv[1], sys.argv[2], sys.argv[3]
conn = sqlite3.connect(db_path)
conn.execute("PRAGMA foreign_keys = ON")
now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

conn.execute(
    "INSERT OR IGNORE INTO tenants(id, name, status, created_at, updated_at) "
    "VALUES(?, ?, 'active', ?, ?)",
    ("default", "default", now, now),
)

def add_user(uid, name):
    conn.execute(
        "INSERT OR REPLACE INTO users("
        "  id, tenant_id, username, display_name, role, status, "
        "  created_at, updated_at) "
        "VALUES(?, 'default', ?, ?, 'super_admin', 'active', ?, ?)",
        (uid, name, name, now, now),
    )

def add_key(kid, uid, raw_token):
    key_hash = hashlib.sha256(raw_token.encode()).hexdigest()
    prefix = raw_token[:8]
    conn.execute(
        "INSERT OR REPLACE INTO api_keys("
        "  id, user_id, tenant_id, name, key_prefix, key_hash, "
        "  role, status, created_at, updated_at) "
        "VALUES(?, ?, 'default', 'it', ?, ?, 'super_admin', 'active', ?, ?)",
        (kid, uid, prefix, key_hash, now, now),
    )

add_user("alice-uid", "alice")
add_user("bob-uid",   "bob")
add_key("alice-key", "alice-uid", alice)
add_key("bob-key",   "bob-uid",   bob)

conn.commit()
conn.close()
print("seeded: alice + bob")
PY

# ---- phase 3: real control-plane boot ---------------------------------------
log "phase 3: restart control-plane"
"$CP_BIN" --config "$CP_CFG" >"$TMP/cp.log" 2>&1 &
CP_PID=$!

for _ in $(seq 1 40); do
    ss -tln 2>/dev/null | grep -q ':19443 ' && break
    sleep 0.5
done
if ! ss -tln 2>/dev/null | grep -q ':19443 '; then
    cat "$TMP/cp.log" >&2
    die "control-plane (main boot) failed to bind :19443"
fi
log "control-plane ready (pid=$CP_PID)"

# ---- phase 4: optional data-plane boot --------------------------------------
if [[ "$DP_AVAILABLE" -eq 1 ]]; then
    # Seed the target with the initial yaml so the data plane has something to
    # load on startup; activate will later overwrite this file atomically.
    cp "$DP_SEED" "$DP_TARGET"
    log "booting data-plane (target=$DP_TARGET)"
    "$DP_BIN" "$DP_TARGET" >"$TMP/dp.log" 2>&1 &
    DP_PID=$!
    sleep 2
    if ! kill -0 "$DP_PID" 2>/dev/null; then
        tail -30 "$TMP/dp.log" >&2 || true
        log "note: data-plane exited early; Verify B will fall back to file-only check"
        DP_PID=0
        DP_AVAILABLE=0
    else
        log "data-plane ready (pid=$DP_PID)"
    fi
fi

# ---- dev hook: pause for cleanup test ---------------------------------------
if [[ -n "${AEGISGATE_IT_PAUSE_AFTER_BOOT:-}" ]]; then
    log "boot-pause start ($AEGISGATE_IT_PAUSE_AFTER_BOOT s)"
    sleep "$AEGISGATE_IT_PAUSE_AFTER_BOOT"
    log "boot-pause end"
fi

# ---- aegisctl environment ---------------------------------------------------
export AEGISGATE_CP_ENDPOINT="127.0.0.1:19443"
export AEGISGATE_CP_TLS_CA="$TMP/certs/ca.crt"

ctl() {
    # Run aegisctl with the CA pinned. Output goes to stdout for the caller
    # to capture; stderr bleeds through to ours.
    "$CTL_BIN" "$@"
}

# ---- apply (alice) ---------------------------------------------------------
log "[alice] config apply"
export AEGISGATE_CP_API_KEY="$ALICE_TOKEN"
apply_out="$(ctl config apply "$DP_SEED" --comment 'IT: initial apply' || {
    log "apply output:"; sed 's/^/    /' "$TMP/cp.log" >&2; exit 3; })"
VID="$(awk '/^version_id:/ { print $2 }' <<<"$apply_out" | head -n1)"
[[ -n "$VID" ]] || { printf '%s\n' "$apply_out" >&2; die "apply: empty version_id"; }
log "submitted version_id=$VID"

# ---- approve (bob) ---------------------------------------------------------
log "[bob] config approve $VID"
export AEGISGATE_CP_API_KEY="$BOB_TOKEN"
ctl config approve "$VID" --comment 'IT: approve' >/dev/null

# ---- activate (alice) ------------------------------------------------------
log "[alice] config activate $VID -> $DP_TARGET  (signal pid=$DP_PID)"
export AEGISGATE_CP_API_KEY="$ALICE_TOKEN"
activate_args=(config activate "$VID" --comment 'IT: activate'
               --data-plane-config-path "$DP_TARGET")
if [[ "$DP_PID" -gt 0 ]]; then
    activate_args+=(--signal-pid "$DP_PID")
fi
ctl "${activate_args[@]}" >/dev/null
sleep 1  # allow data-plane (if running) to process SIGHUP

# ---- Verify A: gRPC state ---------------------------------------------------
log "verify A: config current reports $VID"
current_out="$(ctl config current)"
cur_vid="$(awk '/^version_id:/ { print $2 }' <<<"$current_out" | head -n1)"
if [[ "$cur_vid" != "$VID" ]]; then
    printf '%s\n' "$current_out" >&2
    die "Verify A FAILED: expected $VID, got '$cur_vid'" 1
fi
log "  Verify A OK"

# ---- Verify B: data-plane yaml sha256 ---------------------------------------
log "verify B: sha256(dp target) == sha256(aegisctl config show $VID)"
show_tmp="$TMP/show.yaml"
ctl config show "$VID" > "$show_tmp"
# aegisctl's `config show` prints the raw yaml_content verbatim. A trailing
# newline is appended if the original didn't end in '\n' — that normalisation
# is part of the CLI contract, so we match on the on-disk file bytes after
# the same normalisation on the target side too.
local_sha="$(sha256sum "$DP_TARGET" | awk '{print $1}')"
server_sha="$(sha256sum "$show_tmp" | awk '{print $1}')"
if [[ "$local_sha" != "$server_sha" ]]; then
    log "  local  ($DP_TARGET): $local_sha"
    log "  server (config show):  $server_sha"
    log "  first diff:"
    diff "$DP_TARGET" "$show_tmp" | head -20 >&2 || true
    die "Verify B FAILED: sha256 mismatch" 2
fi
log "  Verify B OK (sha256=$local_sha)"

log "PASS — end-to-end round-trip succeeded for version $VID"
exit 0
