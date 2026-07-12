#!/usr/bin/env bash
# Tests for scripts/check-doc-links.sh
# Style: pure bash assert (no external test framework), follows
# tests/scripts/test-check-windows-proxy.sh pattern.
#
# Usage: bash tests/scripts/test-check-doc-links.sh
# Exit:  0 = all pass, 1 = any fail
set -uo pipefail   # NB: not -e; we want to capture failures explicitly

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TARGET="$ROOT_DIR/scripts/check-doc-links.sh"
FIXTURES="$ROOT_DIR/tests/scripts/fixtures/doc-links"

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

[[ -f "$TARGET" ]] || { echo "[FATAL] target missing: $TARGET"; exit 1; }
[[ -d "$FIXTURES" ]] || { echo "[FATAL] fixtures missing: $FIXTURES"; exit 1; }

chmod +x "$TARGET"

echo "==> T1: --help shows usage and exits 0"
out=$(bash "$TARGET" --help 2>&1); rc=$?
if [[ $rc -eq 0 ]] && [[ "$out" == *"Usage:"* ]]; then
    pass "T1"
else
    fail "T1 (rc=$rc, out=$out)"
fi

echo "==> T2 (positive sample): has-dead.md triggers exit 2 + reports missing file"
out=$(bash "$TARGET" "$FIXTURES/has-dead.md" 2>&1); rc=$?
if [[ $rc -eq 2 ]] && [[ "$out" == *"nonexistent.md"* ]] && [[ "$out" == *"missing/file.md"* ]]; then
    pass "T2 (rc=$rc, reports 2 dead links)"
else
    fail "T2 (rc=$rc, out=$out)"
fi

echo "==> T3 (negative sample / A13 reverse validation): all-good.md exits 0, no dead links"
out=$(bash "$TARGET" "$FIXTURES/all-good.md" 2>&1); rc=$?
if [[ $rc -eq 0 ]] && [[ "$out" == *"PASS"* ]]; then
    pass "T3 (rc=$rc, all resolved)"
else
    fail "T3 (rc=$rc, out=$out)"
fi

echo "==> T4 (real-doc integration): AegisOps positioning + compliance + ROADMAP_v4"
out=$(bash "$TARGET" \
        "$ROOT_DIR/docs/positioning/aegisops-vision.md" \
        "$ROOT_DIR/docs/positioning/aegisops-vision_zh.md" \
        "$ROOT_DIR/docs/compliance/README.md" \
        "$ROOT_DIR/docs/compliance/README_zh.md" \
        "$ROOT_DIR/docs/ROADMAP_v4.md" \
        "$ROOT_DIR/docs/ROADMAP_v4_zh.md" 2>&1); rc=$?
if [[ $rc -eq 0 ]]; then
    pass "T4 (rc=0, AegisOps docs all-good)"
else
    fail "T4 (rc=$rc, out=$out)"
fi

echo "==> T5: mixed input (good + dead) exits 2"
out=$(bash "$TARGET" "$FIXTURES/all-good.md" "$FIXTURES/has-dead.md" 2>&1); rc=$?
if [[ $rc -eq 2 ]]; then
    pass "T5 (rc=$rc, mixed -> ERROR)"
else
    fail "T5 (rc=$rc, out=$out)"
fi

echo "==> T6 (SR1): no LAN IP literal in script source"
if grep -qE '192\.168\.[0-9]+\.[0-9]+' "$TARGET"; then
    fail "T6 (SR1 violated: hardcoded IP)"
else
    pass "T6 (no IP literal)"
fi

echo "==> T7: directory recursion finds *.md files"
out=$(bash "$TARGET" "$FIXTURES" 2>&1); rc=$?
# fixtures dir contains has-dead.md so exit must be 2
if [[ $rc -eq 2 ]] && [[ "$out" == *"nonexistent.md"* ]]; then
    pass "T7 (dir recursion works)"
else
    fail "T7 (rc=$rc, out=$out)"
fi

echo "==> T8: --exclude filter skips matched files"
out=$(bash "$TARGET" --exclude 'has-dead' "$FIXTURES" 2>&1); rc=$?
if [[ $rc -eq 0 ]]; then
    pass "T8 (exclude works)"
else
    fail "T8 (rc=$rc, out=$out)"
fi

echo ""
echo "[SUMMARY] PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]]
