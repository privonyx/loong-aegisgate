#!/usr/bin/env bash
# Tests for scripts/check-doc-wording.sh
# Style: pure bash assert.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TARGET="$ROOT_DIR/scripts/check-doc-wording.sh"
FIXTURES="$ROOT_DIR/tests/scripts/fixtures/doc-wording"

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

echo "==> T2 (positive sample): has-banned.md with banned=compliant,certified,production-grade exits 1"
out=$(bash "$TARGET" --banned 'compliant,certified,production-grade' \
        --exempt-pattern 'disclaimer|does not' \
        "$FIXTURES/has-banned.md" 2>&1); rc=$?
if [[ $rc -eq 1 ]] && [[ "$out" == *"compliant"* ]] && [[ "$out" == *"certified"* ]] && [[ "$out" == *"production-grade"* ]]; then
    pass "T2 (rc=1, all 3 violations reported)"
else
    fail "T2 (rc=$rc, out=$out)"
fi

echo "==> T3 (negative sample / A13): disclaimer-only.md exits 0 (lines exempted)"
out=$(bash "$TARGET" --banned 'compliant,certified,enterprise-ready' \
        --exempt-pattern 'disclaimer|does not|aspirational' \
        "$FIXTURES/disclaimer-only.md" 2>&1); rc=$?
if [[ $rc -eq 0 ]] && [[ "$out" == *"PASS"* ]]; then
    pass "T3 (rc=0, disclaimer exempted)"
else
    fail "T3 (rc=$rc, out=$out)"
fi

echo "==> T4 (real-doc integration): AegisOps docs/positioning + docs/compliance exit 0 (clean)"
out=$(bash "$TARGET" \
        --banned 'compliant,certified,production-grade,enterprise-ready' \
        --exempt-pattern 'disclaimer|不构成|aspirational|conformity|placeholder|certification body|certification pursued|never overstate|绝不|aligns with|认证机构|目标）|"compliant"|"certified"|"production-grade"|~~|readiness 而非|readiness vs' \
        "$ROOT_DIR/docs/positioning/aegisops-vision.md" \
        "$ROOT_DIR/docs/positioning/aegisops-vision_zh.md" \
        "$ROOT_DIR/docs/compliance/README.md" \
        "$ROOT_DIR/docs/compliance/README_zh.md" \
        "$ROOT_DIR/docs/ROADMAP_v4.md" \
        "$ROOT_DIR/docs/ROADMAP_v4_zh.md" 2>&1); rc=$?
if [[ $rc -eq 0 ]]; then
    pass "T4 (rc=0, AegisOps docs clean with K2 wording policy)"
else
    fail "T4 (rc=$rc, out=$out)"
fi

echo "==> T5: --severity 2 elevates exit code from 1 to 2"
out=$(bash "$TARGET" --banned 'compliant' --severity 2 \
        "$FIXTURES/has-banned.md" 2>&1); rc=$?
if [[ $rc -eq 2 ]]; then
    pass "T5 (rc=2, severity elevated to ERROR)"
else
    fail "T5 (rc=$rc, out=$out)"
fi

echo "==> T6: --banned missing triggers exit 1 with [FAIL]"
out=$(bash "$TARGET" "$FIXTURES/has-banned.md" 2>&1); rc=$?
if [[ $rc -eq 1 ]] && [[ "$out" == *"--banned is required"* ]]; then
    pass "T6 (missing flag rejected)"
else
    fail "T6 (rc=$rc, out=$out)"
fi

echo "==> T7: word boundary — 'enterprise scenarios' does NOT match 'enterprise-ready'"
# disclaimer-only.md has 'enterprise scenarios' but not 'enterprise-ready'
out=$(bash "$TARGET" --banned 'enterprise-ready' \
        --exempt-pattern 'disclaimer' \
        "$FIXTURES/disclaimer-only.md" 2>&1); rc=$?
if [[ $rc -eq 0 ]]; then
    pass "T7 (word boundary correct, no false positive)"
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
