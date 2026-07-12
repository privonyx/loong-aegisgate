#!/usr/bin/env bash
# Smoke / e2e tests for `aegisctl estimate` (MVP-2 / TASK-20260526-01).
#
# 8 checks (E1-E8):
#   E1 --help lists --scenario
#   E2 full flags -> exit 0 + 5 SR4 key texts present
#   E3 --output json parses cleanly
#   E4 --model not-exist -> exit 1 + "not found"
#   E5 --monthly-calls 0 -> non-fatal warn-style note
#   E6 --cache-hit-rate 1.5 -> exit 1
#   E7 (SR3) output grep api_key|tenant_id|sk-|password -> 0 hits
#   E8 (SR4) all 5 key texts hard-coded in spec/plan/impl/test/docs
#
# All checks run inside the Cursor sandbox; estimate never touches the network.

set -euo pipefail

# Locate aegisctl binary (from build dir or PATH)
AEGISCTL_BIN="${AEGISCTL_BIN:-build/aegisctl}"
if [ ! -x "$AEGISCTL_BIN" ]; then
    if command -v aegisctl >/dev/null 2>&1; then
        AEGISCTL_BIN="aegisctl"
    else
        echo "ERROR: aegisctl binary not found at $AEGISCTL_BIN and not on PATH"
        echo "Hint: cmake --build build --target aegisctl"
        exit 2
    fi
fi

PASS=0
FAIL=0
TOTAL=0

run_check() {
    local label="$1"; shift
    TOTAL=$((TOTAL + 1))
    if "$@"; then
        echo "[PASS] $label"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] $label"
        FAIL=$((FAIL + 1))
    fi
}

# --- E1 --help lists --scenario ---------------------------------------------
check_e1() {
    local out
    out=$("$AEGISCTL_BIN" estimate --help 2>&1)
    echo "$out" | grep -Fq -- "--scenario"
}

# --- E2 full flags -> 5 SR4 key texts present -------------------------------
check_e2() {
    local out
    if ! out=$("$AEGISCTL_BIN" estimate \
        --model gpt-4o \
        --monthly-calls 100000 \
        --avg-input-tokens 800 \
        --avg-output-tokens 200 2>&1); then
        echo "$out"
        return 1
    fi
    # SR4 5 hard-coded texts must all appear
    local key_texts=(
        "AegisGate Savings Estimate"
        "Estimated monthly savings:"
        "Estimated annual savings:"
        "Want to verify?"
        "docs/quickstart.md"
    )
    for t in "${key_texts[@]}"; do
        if ! grep -Fq "$t" <<<"$out"; then
            echo "  missing key text: $t"
            return 1
        fi
    done
}

# --- E3 --output json parses cleanly ----------------------------------------
check_e3() {
    local out
    out=$("$AEGISCTL_BIN" estimate \
        --model gpt-4o \
        --monthly-calls 100000 \
        --avg-input-tokens 800 \
        --avg-output-tokens 200 \
        --output json 2>&1)
    # python3 if available, else jq, else fallback grep
    if command -v python3 >/dev/null 2>&1; then
        echo "$out" | python3 -c "import json,sys; d=json.loads(sys.stdin.read()); \
            assert 'baseline' in d and 'savings' in d, 'missing keys'"
    elif command -v jq >/dev/null 2>&1; then
        echo "$out" | jq -e '.baseline.monthly_total' >/dev/null
    else
        # Minimal fallback: must look like JSON
        grep -Fq '"baseline"' <<<"$out" && grep -Fq '"savings"' <<<"$out"
    fi
}

# --- E4 unknown model -> exit 1 + "not found" -------------------------------
check_e4() {
    local out rc
    set +e
    out=$("$AEGISCTL_BIN" estimate \
        --model nonexistent-model-xyz \
        --monthly-calls 1000 \
        --avg-input-tokens 100 \
        --avg-output-tokens 50 2>&1)
    rc=$?
    set -e
    [ "$rc" -eq 1 ] && grep -Fq "not found" <<<"$out"
}

# --- E5 monthly-calls 0 -> exit 0 + zero numbers ----------------------------
check_e5() {
    local out
    out=$("$AEGISCTL_BIN" estimate \
        --model gpt-4o \
        --monthly-calls 0 \
        --avg-input-tokens 100 \
        --avg-output-tokens 50 2>&1)
    # Total saved must be zero on this path
    grep -Fq "no estimate possible" <<<"$out"
}

# --- E6 cache-hit-rate 1.5 -> exit 1 ----------------------------------------
check_e6() {
    local rc
    set +e
    "$AEGISCTL_BIN" estimate \
        --model gpt-4o \
        --monthly-calls 1000 \
        --avg-input-tokens 100 \
        --avg-output-tokens 50 \
        --cache-hit-rate 1.5 >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 1 ]
}

# --- E7 SR3: output 0 sensitive markers (use temp file, no pipefail trap) ---
check_e7_sr3() {
    # N1 walk-through grep pipefail mitigation: capture to temp file, then grep.
    local tmp
    tmp=$(mktemp)
    "$AEGISCTL_BIN" estimate \
        --model gpt-4o \
        --monthly-calls 100000 \
        --avg-input-tokens 800 \
        --avg-output-tokens 200 \
        --explain >"$tmp" 2>&1
    local hits
    # grep -E with a pipefail-safe pattern; -c prints count, never returns 1.
    hits=$(grep -cE '(api_key|tenant_id|sk-[A-Za-z0-9]{4}|password=)' "$tmp" || true)
    rm -f "$tmp"
    [ "$hits" -eq 0 ]
}

# --- E8 SR4: 5 key texts present in spec/plan/impl/test/docs (5-way share) --
# Note: docs/estimate.md is delivered in Epic 4; this E8 here only checks
# the 4 sources that exist by Epic 3 end. Epic 5 task 5.3 re-runs the full
# 5-way share audit including docs.
check_e8_sr4() {
    local sources=(
        "docs/specs/2026-05-26-mvp2-aegisctl-estimate-design.md"
        "docs/plans/2026-05-26-mvp2-aegisctl-estimate.md"
        "src/cli/estimate_cli.cpp"
        "scripts/test-estimate-cli.sh"
    )
    local key_texts=(
        "AegisGate Savings Estimate"
        "Estimated monthly savings:"
        "Estimated annual savings:"
        "Want to verify?"
    )
    local fail=0
    for t in "${key_texts[@]}"; do
        local count=0
        for s in "${sources[@]}"; do
            if [ -f "$s" ] && grep -Fq "$t" "$s"; then
                count=$((count + 1))
            fi
        done
        # All 4 sources must contain each key text (spec lock + 3-way share)
        if [ "$count" -lt 4 ]; then
            echo "  key text \"$t\" present in only $count/4 sources"
            fail=1
        fi
    done
    [ "$fail" -eq 0 ]
}

echo "=== aegisctl estimate smoke tests ($(date -u +%FT%TZ)) ==="
echo "Binary: $AEGISCTL_BIN"
echo

run_check "E1 --help lists --scenario"            check_e1
run_check "E2 full flags + 5 SR4 key texts"       check_e2
run_check "E3 --output json parses cleanly"       check_e3
run_check "E4 unknown model -> exit 1"            check_e4
run_check "E5 monthly-calls=0 -> warn note"       check_e5
run_check "E6 cache-hit-rate 1.5 -> exit 1"       check_e6
run_check "E7 SR3: output 0 sensitive markers"    check_e7_sr3
run_check "E8 SR4: 5 key texts shared (4-way)"    check_e8_sr4

echo
echo "=== Result: $PASS PASS / $FAIL FAIL / $TOTAL TOTAL ==="
[ "$FAIL" -eq 0 ]
