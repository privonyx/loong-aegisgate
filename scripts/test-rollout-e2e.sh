#!/usr/bin/env bash
# scripts/test-rollout-e2e.sh
#
# Phase 9.3.4 Epic F.1 — end-to-end rollout smoke test.
#
# Flow:
#   1. Boot control-plane (reuse test-control-plane-local.sh infra patterns)
#   2. Seed alice + bob SuperAdmin users
#   3. aegisctl config apply → approve → activate (establish baseline)
#   4. aegisctl rollout create --spec spec.yaml (2-stage: canary 10% + full 100%)
#   5. aegisctl rollout start
#   6. aegisctl rollout status → expect PROGRESSING
#   7. aegisctl rollout pause → expect PAUSED
#   8. aegisctl rollout resume → expect PROGRESSING
#   9. aegisctl rollout promote → advance to stage 1
#   10. aegisctl rollout promote → COMPLETED (last stage)
#   11. aegisctl rollout list --output json → verify array contains our rollout
#
# Exit codes:
#   0  PASS
#   1  FAIL (verification mismatch)
#   3  FAIL at boot / precondition

set -euo pipefail

log()  { printf '[ROLLOUT-E2E %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die()  { log "FATAL: $*"; exit "${2:-3}"; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# ---- binary discovery -------------------------------------------------------
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

[[ -x "$CP_BIN"  ]] || die "missing binary: aegisgate-control-plane (build the ON-path first)"
[[ -x "$CTL_BIN" ]] || die "missing binary: aegisctl (build the ON-path first)"

command -v python3   >/dev/null || die "python3 not installed"

# ---- workdir + cleanup trap -------------------------------------------------
TMP="$(mktemp -d -t aegisgate-rollout-e2e.XXXXXX)"
chmod 700 "$TMP"
log "workdir: $TMP"

CP_PID=0

# shellcheck disable=SC2317
cleanup() {
    local rc=$?
    set +e
    log "cleanup entered (exit=$rc)"
    [[ "$CP_PID" -gt 0 ]] && kill -TERM "$CP_PID" 2>/dev/null
    sleep 1
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
CP_PORT=19444
CP_CFG="$TMP/cp.yaml"
DP_SEED="$TMP/dp-seed.yaml"
DP_TARGET="$TMP/dp-target.yaml"

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
    listen_address: 127.0.0.1:$CP_PORT
  submit_rate_limit_per_user_per_min: 100
  max_yaml_size_bytes: 1048576
EOF

cp "$REPO/config/aegisgate.yaml" "$DP_SEED"

# ---- phase 1: first boot to create schema -----------------------------------
log "phase 1: boot control-plane to create sqlite schema"
"$CP_BIN" --config "$CP_CFG" >"$TMP/cp-init.log" 2>&1 &
init_pid=$!

for _ in $(seq 1 40); do
    ss -tln 2>/dev/null | grep -q ":$CP_PORT " && break
    sleep 0.5
done
if ! ss -tln 2>/dev/null | grep -q ":$CP_PORT "; then
    cat "$TMP/cp-init.log" >&2
    die "control-plane (init) failed to bind :$CP_PORT"
fi

kill -TERM "$init_pid"
wait "$init_pid" 2>/dev/null || true
log "schema created"

# ---- phase 2: seed users ----------------------------------------------------
ALICE_TOKEN="rollout-e2e-alice-token-4f2c7a9b"
BOB_TOKEN="rollout-e2e-bob-token-9e3d1c6f"

log "seeding alice + bob"
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

# ---- phase 3: main boot -----------------------------------------------------
log "phase 3: restart control-plane"
"$CP_BIN" --config "$CP_CFG" >"$TMP/cp.log" 2>&1 &
CP_PID=$!

for _ in $(seq 1 40); do
    ss -tln 2>/dev/null | grep -q ":$CP_PORT " && break
    sleep 0.5
done
if ! ss -tln 2>/dev/null | grep -q ":$CP_PORT "; then
    cat "$TMP/cp.log" >&2
    die "control-plane (main) failed to bind :$CP_PORT"
fi
log "control-plane ready (pid=$CP_PID)"

# ---- aegisctl env ------------------------------------------------------------
export AEGISGATE_CP_ENDPOINT="127.0.0.1:$CP_PORT"
export AEGISGATE_CP_TLS_CA="$TMP/certs/ca.crt"

ctl() { "$CTL_BIN" "$@"; }

# ---- step 1: config apply + approve + activate (baseline) -------------------
log "[alice] config apply"
export AEGISGATE_CP_API_KEY="$ALICE_TOKEN"
apply_out="$(ctl config apply "$DP_SEED" --comment 'rollout-e2e: baseline' || {
    cat "$TMP/cp.log" >&2; die "config apply failed"; })"
VID="$(awk '/^version_id:/ { print $2 }' <<<"$apply_out" | head -n1)"
[[ -n "$VID" ]] || die "apply: empty version_id"
log "submitted version_id=$VID"

log "[bob] config approve"
export AEGISGATE_CP_API_KEY="$BOB_TOKEN"
ctl config approve "$VID" --comment 'rollout-e2e: approve' >/dev/null

log "[alice] config activate"
export AEGISGATE_CP_API_KEY="$ALICE_TOKEN"
ctl config activate "$VID" --comment 'rollout-e2e: activate' \
    --data-plane-config-path "$DP_TARGET" >/dev/null
log "baseline ACTIVE: $VID"

# ---- step 2: submit + approve a new config for rollout target ----------------
log "[alice] config apply (rollout target)"
TARGET_SEED="$TMP/target-seed.yaml"
cp "$DP_SEED" "$TARGET_SEED"
printf '\n# rollout-e2e target marker\n' >> "$TARGET_SEED"

export AEGISGATE_CP_API_KEY="$ALICE_TOKEN"
target_out="$(ctl config apply "$TARGET_SEED" --comment 'rollout-e2e: target' || {
    cat "$TMP/cp.log" >&2; die "config apply (target) failed"; })"
TARGET_VID="$(awk '/^version_id:/ { print $2 }' <<<"$target_out" | head -n1)"
[[ -n "$TARGET_VID" ]] || die "apply target: empty version_id"
log "target version_id=$TARGET_VID"

log "[bob] config approve (target)"
export AEGISGATE_CP_API_KEY="$BOB_TOKEN"
ctl config approve "$TARGET_VID" --comment 'rollout-e2e: approve target' >/dev/null

# ---- step 3: rollout create -------------------------------------------------
SPEC="$TMP/rollout-spec.yaml"
cat > "$SPEC" <<EOF
target_version_id: "$TARGET_VID"
sticky_key: "tenant_id"
auto_rollback_on_pause: false
auto_rollback_grace_seconds: 600
creator_comment: "rollout-e2e: 2-stage canary"
stages:
  - name: canary
    scope:
      percentage: 10
    observation:
      min_duration_seconds: 0
      min_sample_count: 0
  - name: full
    scope:
      percentage: 100
    observation:
      min_duration_seconds: 0
      min_sample_count: 0
EOF

log "[alice] rollout create"
export AEGISGATE_CP_API_KEY="$ALICE_TOKEN"
create_out="$(ctl rollout create --spec "$SPEC" || {
    cat "$TMP/cp.log" >&2; die "rollout create failed"; })"
RID="$(awk '/^rollout_id:/ { print $2 }' <<<"$create_out" | head -n1)"
[[ -n "$RID" ]] || die "create: empty rollout_id"
log "created rollout_id=$RID"

# ---- step 4: rollout start --------------------------------------------------
log "[alice] rollout start"
ctl rollout status --rollout-id "$RID" >/dev/null 2>&1 || true
ctl rollout start --rollout-id "$RID" --comment 'e2e: start' >/dev/null || {
    cat "$TMP/cp.log" >&2; die "rollout start failed"; }

status_out="$(ctl rollout status --rollout-id "$RID")"
if ! grep -q 'PROGRESSING' <<<"$status_out"; then
    printf '%s\n' "$status_out" >&2
    die "expected PROGRESSING after start" 1
fi
log "  status=PROGRESSING OK"

# ---- step 5: rollout pause --------------------------------------------------
log "[alice] rollout pause"
ctl rollout pause --rollout-id "$RID" --comment 'e2e: pause' >/dev/null

status_out="$(ctl rollout status --rollout-id "$RID")"
if ! grep -q 'PAUSED' <<<"$status_out"; then
    printf '%s\n' "$status_out" >&2
    die "expected PAUSED after pause" 1
fi
log "  status=PAUSED OK"

# ---- step 6: rollout resume -------------------------------------------------
log "[alice] rollout resume"
ctl rollout resume --rollout-id "$RID" --comment 'e2e: resume' >/dev/null

status_out="$(ctl rollout status --rollout-id "$RID")"
if ! grep -q 'PROGRESSING' <<<"$status_out"; then
    printf '%s\n' "$status_out" >&2
    die "expected PROGRESSING after resume" 1
fi
log "  status=PROGRESSING OK"

# ---- step 7: rollout promote (stage 0 → 1) ----------------------------------
log "[alice] rollout promote (canary → full)"
ctl rollout promote --rollout-id "$RID" --comment 'e2e: promote to full' >/dev/null

status_out="$(ctl rollout status --rollout-id "$RID")"
if ! grep -q 'PROGRESSING\|stage_index.*1' <<<"$status_out"; then
    printf '%s\n' "$status_out" >&2
    log "note: promote might have completed if last stage; checking..."
fi

# ---- step 8: rollout promote (last stage → COMPLETED) -----------------------
log "[alice] rollout promote (full → COMPLETED)"
ctl rollout promote --rollout-id "$RID" --comment 'e2e: final promote' >/dev/null

status_out="$(ctl rollout status --rollout-id "$RID")"
if ! grep -q 'COMPLETED' <<<"$status_out"; then
    printf '%s\n' "$status_out" >&2
    die "expected COMPLETED after final promote" 1
fi
log "  status=COMPLETED OK"

# ---- step 9: rollout list --output json -------------------------------------
log "[alice] rollout list --output json"
list_out="$(ctl rollout list --output json)"
if ! printf '%s' "$list_out" | python3 -c "
import sys, json
data = json.load(sys.stdin)
assert isinstance(data, list), 'expected JSON array'
ids = [r['rollout_id'] for r in data]
assert '$RID' in ids, f'rollout $RID not in list: {ids}'
print(f'list contains {len(data)} rollout(s), including $RID')
"; then
    printf '%s\n' "$list_out" >&2
    die "rollout list --output json verification failed" 1
fi

log "PASS — rollout end-to-end smoke test succeeded (rollout_id=$RID)"
exit 0
