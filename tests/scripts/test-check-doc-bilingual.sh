#!/usr/bin/env bash
# Tests for scripts/check-doc-bilingual.sh
# Style: pure bash assert.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TARGET="$ROOT_DIR/scripts/check-doc-bilingual.sh"
FIXTURES="$ROOT_DIR/tests/scripts/fixtures/doc-bilingual"

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

echo "==> T2 (positive sample = symmetric PASS): sym.md + sym_zh.md exits 0"
out=$(bash "$TARGET" "$FIXTURES/sym.md" "$FIXTURES/sym_zh.md" 2>&1); rc=$?
if [[ $rc -eq 0 ]] && [[ "$out" == *"PASS"* ]]; then
    pass "T2 (rc=0, symmetric)"
else
    fail "T2 (rc=$rc, out=$out)"
fi

echo "==> T3 (negative sample / A13): mismatch.md + mismatch_zh.md exits 1"
out=$(bash "$TARGET" "$FIXTURES/mismatch.md" "$FIXTURES/mismatch_zh.md" 2>&1); rc=$?
if [[ $rc -eq 1 ]] && [[ "$out" == *"MISMATCH"* ]]; then
    pass "T3 (rc=1, mismatch reported)"
else
    fail "T3 (rc=$rc, out=$out)"
fi

echo "==> T4 (real-doc integration): AegisOps 3 pairs in docs/positioning + docs/compliance"
out=$(bash "$TARGET" \
        "$ROOT_DIR/docs/positioning/aegisops-vision.md" "$ROOT_DIR/docs/positioning/aegisops-vision_zh.md" \
        "$ROOT_DIR/docs/compliance/README.md" "$ROOT_DIR/docs/compliance/README_zh.md" \
        "$ROOT_DIR/docs/ROADMAP_v4.md" "$ROOT_DIR/docs/ROADMAP_v4_zh.md" 2>&1); rc=$?
if [[ $rc -eq 0 ]] && [[ "$out" == *"3/3 pairs symmetric"* ]]; then
    pass "T4 (rc=0, all 3 AegisOps pairs symmetric)"
else
    fail "T4 (rc=$rc, out=$out)"
fi

echo "==> T5: --auto-discover on fixtures dir finds both sym and mismatch pairs"
out=$(bash "$TARGET" --auto-discover "$FIXTURES" 2>&1); rc=$?
# fixtures has 1 symmetric pair + 1 mismatched pair => exit 1
if [[ $rc -eq 1 ]] && [[ "$out" == *"1/2 pairs symmetric"* ]]; then
    pass "T5 (auto-discover correctly identified 1 sym + 1 mismatch)"
else
    fail "T5 (rc=$rc, out=$out)"
fi

echo "==> T6: --pairs file input"
tmp=$(mktemp)
cat > "$tmp" <<EOF
# comment line, ignored
$FIXTURES/sym.md $FIXTURES/sym_zh.md

$FIXTURES/sym.md $FIXTURES/sym_zh.md
EOF
out=$(bash "$TARGET" --pairs "$tmp" 2>&1); rc=$?
rm -f "$tmp"
if [[ $rc -eq 0 ]] && [[ "$out" == *"2/2 pairs symmetric"* ]]; then
    pass "T6 (pairs file works, 2/2)"
else
    fail "T6 (rc=$rc, out=$out)"
fi

echo "==> T7: missing file path triggers exit 1 with [FAIL]"
out=$(bash "$TARGET" "$FIXTURES/nonexistent.md" "$FIXTURES/sym_zh.md" 2>&1); rc=$?
if [[ $rc -eq 1 ]] && [[ "$out" == *"missing en file"* ]]; then
    pass "T7 (missing file reported)"
else
    fail "T7 (rc=$rc, out=$out)"
fi

echo "==> T8 (SR1): no LAN IP literal in script source"
if grep -qE '192\.168\.[0-9]+\.[0-9]+' "$TARGET"; then
    fail "T8 (SR1 violated: hardcoded IP)"
else
    pass "T8 (no IP literal)"
fi

echo ""
echo "[SUMMARY] PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]]
