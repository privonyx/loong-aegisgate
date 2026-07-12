#!/usr/bin/env bash
# Tests for scripts/check-windows-proxy.sh
# Style: pure bash assert (no external test framework), follows
# scripts/test-admin-panel-smoke.sh pattern.
#
# Usage: bash tests/scripts/test-check-windows-proxy.sh
# Exit:  0 = all pass, 1 = any fail
set -uo pipefail   # NB: not -e; we want to capture failures explicitly

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TARGET="$ROOT_DIR/scripts/check-windows-proxy.sh"

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

[[ -f "$TARGET" ]] || { echo "[FATAL] target missing: $TARGET"; exit 1; }

# Cursor sandbox / many CI containers run as root; the script's SR4 guard would
# otherwise abort every test. We opt-in to the documented escape hatch here so
# the *behavioural* tests can run, while T5 still verifies the guard's source.
export WINDOWS_PROXY_CHECK_ALLOW_ROOT=1

echo "==> T1: --help shows usage and exits 0"
out=$(bash "$TARGET" --help 2>&1); rc=$?
if [[ $rc -eq 0 ]] && [[ "$out" == *"Usage:"* ]]; then
    pass "T1"
else
    fail "T1 (rc=$rc, out=$out)"
fi

echo "==> T2: no env var -> exit 1 with 'not configured' hint"
out=$(env -u WINDOWS_PROXY_HOST bash "$TARGET" 2>&1); rc=$?
if [[ $rc -eq 1 ]] && [[ "$out" == *"未配置"* || "$out" == *"not configured"* ]]; then
    pass "T2"
else
    fail "T2 (rc=$rc, out=$out)"
fi

echo "==> T3: env var set + --full + closed port -> exit 1, [WARN]"
out=$(WINDOWS_PROXY_HOST=127.0.0.1:1 bash "$TARGET" --full 2>&1); rc=$?
if [[ $rc -eq 1 ]] && [[ "$out" == *"[WARN]"* ]]; then
    pass "T3"
else
    fail "T3 (rc=$rc, out=$out)"
fi

echo "==> T4: SR2 -- proxy URL with user:pass redacted in logs"
out=$(WINDOWS_PROXY_HOST="http://admin:supersecret@127.0.0.1:1" \
      bash "$TARGET" --full 2>&1); rc=$?
if [[ "$out" == *"supersecret"* ]]; then
    fail "T4 (SR2 violated: 'supersecret' leaked in logs)"
else
    pass "T4 (no leak)"
fi

echo "==> T5: SR4 -- script source contains the no-root guard"
# We cannot really sudo in tests; assert the guard exists in the source.
if grep -q 'EUID' "$TARGET"; then
    pass "T5 (root guard present)"
else
    fail "T5 (no EUID guard in source)"
fi

echo "==> T6: SR3 -- script must use --max-time on probes"
if grep -q -- '--max-time' "$TARGET"; then
    pass "T6 (--max-time present)"
else
    fail "T6 (no --max-time guard, SR3 violated)"
fi

echo "==> T7: SR1 -- no LAN IP literal in script"
if grep -qE '192\.168\.[0-9]+\.[0-9]+' "$TARGET"; then
    fail "T7 (SR1 violated: hardcoded IP)"
else
    pass "T7 (no IP literal)"
fi

echo ""
echo "==> Summary: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
