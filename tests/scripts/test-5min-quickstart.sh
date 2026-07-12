#!/usr/bin/env bash
# End-to-end self-check for MVP-1 5-minute quickstart workflow.
#
# Two modes:
#   1. DRY-RUN (cloud agent / no docker): validates script + yaml syntax,
#      file existence, Dockerfile changes; skips actual `docker run`.
#   2. FULL (host with docker): builds image, runs container, polls health,
#      curls LLM 2x (cache miss + hit), curls savings endpoint, verifies
#      tokens_saved > 0. Total budget: 5 minutes.
#
# Usage:
#   bash tests/scripts/test-5min-quickstart.sh           # auto-detect mode
#   bash tests/scripts/test-5min-quickstart.sh --dry     # force dry-run
#   bash tests/scripts/test-5min-quickstart.sh --full    # force full e2e
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT_DIR"

PASS=0
FAIL=0
SKIP=0
pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }

# --- Mode detection ---
MODE="auto"
if [[ "${1:-}" == "--dry" ]]; then MODE="dry"
elif [[ "${1:-}" == "--full" ]]; then MODE="full"
fi

if [[ "$MODE" == "auto" ]]; then
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        MODE="full"
    else
        MODE="dry"
    fi
fi

echo "==> Mode: $MODE"
echo ""

# ============================================================
# Phase 1 — Static checks (always run, even in dry mode)
# ============================================================

echo "==> S1: scripts/quickstart-entrypoint.sh exists + executable"
if [[ -x "$ROOT_DIR/scripts/quickstart-entrypoint.sh" ]]; then
    pass "S1 (entrypoint script present)"
else
    fail "S1 (missing or not executable)"
fi

echo "==> S2: scripts/quickstart.sh exists + executable"
if [[ -x "$ROOT_DIR/scripts/quickstart.sh" ]]; then
    pass "S2 (wrapper script present)"
else
    fail "S2 (missing or not executable)"
fi

echo "==> S3: config/aegisgate.quickstart.yaml exists, <= 30 lines, valid YAML"
if [[ -f "$ROOT_DIR/config/aegisgate.quickstart.yaml" ]]; then
    LINES=$(wc -l < "$ROOT_DIR/config/aegisgate.quickstart.yaml")
    if [[ $LINES -le 30 ]]; then
        if python3 -c "import yaml; yaml.safe_load(open('$ROOT_DIR/config/aegisgate.quickstart.yaml'))" 2>/dev/null; then
            pass "S3 ($LINES lines, valid yaml)"
        else
            fail "S3 (YAML syntax error)"
        fi
    else
        fail "S3 ($LINES lines, exceeds 30-line budget)"
    fi
else
    fail "S3 (file missing)"
fi

echo "==> S4: Dockerfile copies quickstart-entrypoint.sh to /usr/local/bin/"
if grep -q 'quickstart-entrypoint.sh /usr/local/bin' "$ROOT_DIR/Dockerfile"; then
    pass "S4 (Dockerfile COPY present)"
else
    fail "S4 (Dockerfile missing entrypoint script COPY)"
fi

echo "==> S5: Dockerfile default ENTRYPOINT/CMD UNCHANGED (zero impact on existing users)"
if grep -q 'ENTRYPOINT \["./aegisgate"\]' "$ROOT_DIR/Dockerfile" && \
   grep -q 'CMD \["config/aegisgate.yaml"\]' "$ROOT_DIR/Dockerfile"; then
    pass "S5 (default ENTRYPOINT + CMD preserved)"
else
    fail "S5 (default entry changed — would break existing users)"
fi

echo "==> S6: quickstart-entrypoint.sh bash syntax valid"
if bash -n "$ROOT_DIR/scripts/quickstart-entrypoint.sh"; then
    pass "S6 (bash syntax OK)"
else
    fail "S6 (bash syntax error)"
fi

echo "==> S7: quickstart.sh bash syntax valid"
if bash -n "$ROOT_DIR/scripts/quickstart.sh"; then
    pass "S7 (bash syntax OK)"
else
    fail "S7 (bash syntax error)"
fi

echo "==> S8: SR test (test-quickstart-entrypoint.sh) PASSes"
if bash "$ROOT_DIR/tests/scripts/test-quickstart-entrypoint.sh" >/dev/null 2>&1; then
    pass "S8 (8 SR tests all PASS)"
else
    fail "S8 (SR tests FAIL — see test-quickstart-entrypoint.sh output)"
fi

# ============================================================
# Phase 2 — Live docker e2e (only in --full mode)
# ============================================================

if [[ "$MODE" == "full" ]]; then
    echo ""
    echo "==> Phase 2: live docker e2e (5-minute budget)"

    if [[ -z "${OPENAI_API_KEY:-}" ]]; then
        echo "  [SKIP] OPENAI_API_KEY not set — cannot run real LLM e2e"
        skip "E1-E5 (no OPENAI_API_KEY)"
    else
        START_TIME=$(date +%s)
        CONTAINER="aegisgate-quickstart-e2e-$$"
        IMAGE="${AEGISGATE_IMAGE:-aegisgate:latest}"
        PORT="${AEGISGATE_PORT:-18080}"  # use non-default to avoid collisions

        cleanup() {
            docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
            docker volume rm "aegisgate-quickstart-e2e-data-$$" >/dev/null 2>&1 || true
        }
        trap cleanup EXIT

        echo "==> E1: docker run quickstart container"
        if docker run -d --rm --name "$CONTAINER" \
            -p "${PORT}:8080" \
            -e OPENAI_API_KEY="$OPENAI_API_KEY" \
            -v "aegisgate-quickstart-e2e-data-$$:/app/data" \
            --entrypoint /usr/local/bin/quickstart-entrypoint.sh \
            "$IMAGE" >/dev/null 2>&1; then
            pass "E1 (container started)"
        else
            fail "E1 (docker run failed — is image '$IMAGE' built?)"
            echo ""
            echo "[SUMMARY] PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
            [[ $FAIL -eq 0 ]]; exit
        fi

        echo "==> E2: poll health endpoint (max 60s)"
        READY=0
        for i in {1..30}; do
            if curl -fsS "http://localhost:${PORT}/health/ready" >/dev/null 2>&1; then
                READY=1
                pass "E2 (ready after ${i}*2s)"
                break
            fi
            sleep 2
        done
        if [[ $READY -eq 0 ]]; then
            fail "E2 (not ready after 60s)"
        fi

        if [[ $READY -eq 1 ]]; then
            QUICKSTART_KEY=$(docker exec "$CONTAINER" cat /app/data/quickstart-key.txt 2>/dev/null || echo "")
            if [[ -z "$QUICKSTART_KEY" ]]; then
                fail "E3 (cannot read quickstart key from container)"
            else
                pass "E3 (key extracted: ${QUICKSTART_KEY:0:10}...)"
            fi

            echo "==> E4: first LLM call (cache miss)"
            T1_START=$(date +%s%3N)
            if curl -fsS -X POST "http://localhost:${PORT}/v1/chat/completions" \
                -H "Authorization: Bearer ${QUICKSTART_KEY}" \
                -H "Content-Type: application/json" \
                -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"Say hello in 5 words"}]}' \
                -o /tmp/r1.json 2>&1; then
                T1_MS=$(($(date +%s%3N) - T1_START))
                pass "E4 (1st call OK, latency=${T1_MS}ms)"
            else
                fail "E4 (1st call failed — check OPENAI_API_KEY validity)"
            fi

            echo "==> E5: second identical call (expect cache hit)"
            T2_START=$(date +%s%3N)
            if curl -fsS -X POST "http://localhost:${PORT}/v1/chat/completions" \
                -H "Authorization: Bearer ${QUICKSTART_KEY}" \
                -H "Content-Type: application/json" \
                -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"Say hello in 5 words"}]}' \
                -o /tmp/r2.json 2>&1; then
                T2_MS=$(($(date +%s%3N) - T2_START))
                pass "E5 (2nd call OK, latency=${T2_MS}ms; expect << ${T1_MS}ms for cache hit)"
            else
                fail "E5 (2nd call failed)"
            fi

            echo "==> E6: GET /admin/api/savings/summary"
            if curl -fsS "http://localhost:${PORT}/admin/api/savings/summary" \
                -H "Authorization: Bearer ${QUICKSTART_KEY}" \
                -o /tmp/savings.json 2>&1; then
                SAVINGS=$(python3 -c "import json; d=json.load(open('/tmp/savings.json')); print(d.get('tokens_saved', 0))" 2>/dev/null || echo "0")
                if [[ $SAVINGS -gt 0 ]]; then
                    pass "E6 (savings endpoint OK, tokens_saved=$SAVINGS)"
                else
                    fail "E6 (savings reachable but tokens_saved=$SAVINGS — cache may not have populated)"
                fi
            else
                fail "E6 (savings endpoint failed)"
            fi

            ELAPSED=$(($(date +%s) - START_TIME))
            echo ""
            echo "==> E7: total elapsed = ${ELAPSED}s (5-min budget = 300s)"
            if [[ $ELAPSED -le 300 ]]; then
                pass "E7 (within 5-min budget)"
            else
                fail "E7 (exceeded 5-min budget: ${ELAPSED}s)"
            fi
        fi
    fi
else
    echo ""
    echo "==> Phase 2 SKIPPED (mode=dry; docker unavailable or --dry forced)"
    skip "E1-E7 (docker not available; static checks only)"
fi

echo ""
echo "[SUMMARY] PASS=$PASS FAIL=$FAIL SKIP=$SKIP (mode=$MODE)"
[[ $FAIL -eq 0 ]]
