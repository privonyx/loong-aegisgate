#!/usr/bin/env bash
# Tests for scripts/quickstart-entrypoint.sh
# Style: pure bash assert (no external test framework).
#
# Usage: bash tests/scripts/test-quickstart-entrypoint.sh
# Exit:  0 = all pass, 1 = any fail
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TARGET="$ROOT_DIR/scripts/quickstart-entrypoint.sh"

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

[[ -f "$TARGET" ]] || { echo "[FATAL] target missing: $TARGET"; exit 1; }

chmod +x "$TARGET"

# Sandbox: each test gets isolated tmp dir for key file + fake aegisgate binary
TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

# Create fake aegisgate binary so entrypoint can exec it (just echoes args + exits 0)
FAKE_AEGISGATE="$TMP_ROOT/aegisgate"
cat > "$FAKE_AEGISGATE" <<'FAKE'
#!/usr/bin/env bash
echo "[fake-aegisgate] launched with: $@"
echo "[fake-aegisgate] AEGISGATE_QUICKSTART_API_KEY=${AEGISGATE_QUICKSTART_API_KEY:-(unset)}"
exit 0
FAKE
chmod +x "$FAKE_AEGISGATE"

# Helper: run entrypoint with isolated env + fake aegisgate in PATH.
# Args (all optional): pass env vars as NAME=VALUE strings, e.g.
#   run_entrypoint OPENAI_API_KEY=sk-test
# Returns: combined stdout+stderr. Caller can capture into a variable.
run_entrypoint() {
    local key_file="$TMP_ROOT/test-$RANDOM-key.txt"
    local workdir="$TMP_ROOT/test-$RANDOM-work"
    mkdir -p "$workdir"
    cp "$FAKE_AEGISGATE" "$workdir/aegisgate"

    (
        cd "$workdir"
        # NO_SLEEP for test speed; KEY_FILE isolated per-call
        env -i \
            PATH="$PATH" \
            HOME="$HOME" \
            AEGISGATE_QUICKSTART_NO_SLEEP=1 \
            AEGISGATE_QUICKSTART_KEY_FILE="$key_file" \
            "$@" \
            bash "$TARGET" 2>&1
        # Echo key file path so caller can find it
        echo "QUICKSTART_KEY_FILE_PATH=$key_file"
    )
}

echo "==> T1: --help shows usage and exits 0"
out=$(bash "$TARGET" --help 2>&1); rc=$?
if [[ $rc -eq 0 ]] && [[ "$out" == *"Usage:"* ]] && [[ "$out" == *"AegisGate Quickstart"* ]]; then
    pass "T1 (--help works)"
else
    fail "T1 (rc=$rc, out=${out:0:200})"
fi

echo "==> T2 (SR2): AEGISGATE_PRODUCTION=1 -> exit 1 with 'MUST NOT' message"
out=$(AEGISGATE_PRODUCTION=1 bash "$TARGET" 2>&1); rc=$?
if [[ $rc -eq 1 ]] && [[ "$out" == *"MUST NOT run in production"* ]]; then
    pass "T2 (SR2 production guard works)"
else
    fail "T2 (rc=$rc, expected exit 1 + 'MUST NOT', got out=${out:0:200})"
fi

echo "==> T3 (SR1): auto-gen key is >= 32 bytes base64 (>= 40 chars)"
out=$(run_entrypoint OPENAI_API_KEY=sk-test)
KEY_FILE_T3=$(echo "$out" | grep -oE 'QUICKSTART_KEY_FILE_PATH=[^ ]+' | head -1 | cut -d= -f2)
if [[ -n "$KEY_FILE_T3" ]] && [[ -f "$KEY_FILE_T3" ]]; then
    GENERATED_KEY=$(cat "$KEY_FILE_T3")
    KEY_LEN=${#GENERATED_KEY}
    if [[ $KEY_LEN -ge 40 ]] && [[ "$GENERATED_KEY" =~ ^[A-Za-z0-9+/]+$ ]]; then
        pass "T3 (key format OK: ${KEY_LEN} chars base64)"
    else
        fail "T3 (key length=$KEY_LEN, key=$GENERATED_KEY)"
    fi
else
    fail "T3 (key file not written: $KEY_FILE_T3, out tail=${out: -300})"
fi

echo "==> T4 (SR4): banner contains 'DO NOT use in production' + curl template"
out=$(run_entrypoint OPENAI_API_KEY=sk-test)
if [[ "$out" == *"DO NOT use in production"* ]] && \
   [[ "$out" == *"curl"* ]] && \
   [[ "$out" == *"Authorization: Bearer"* ]]; then
    pass "T4 (banner content OK)"
else
    fail "T4 (banner missing required markers, out tail=${out: -500})"
fi

echo "==> T5 (SR3): key file perm is 600"
out=$(run_entrypoint OPENAI_API_KEY=sk-test)
KEY_FILE_T5=$(echo "$out" | grep -oE 'QUICKSTART_KEY_FILE_PATH=[^ ]+' | head -1 | cut -d= -f2)
if [[ -n "$KEY_FILE_T5" ]] && [[ -f "$KEY_FILE_T5" ]]; then
    PERM=$(stat -c '%a' "$KEY_FILE_T5")
    if [[ "$PERM" == "600" ]]; then
        pass "T5 (file perm = 600)"
    else
        fail "T5 (perm=$PERM, expected 600)"
    fi
else
    fail "T5 (key file not created: $KEY_FILE_T5)"
fi

echo "==> T6: second run reuses existing key (not regenerated)"
# Use shared key file across two runs
SHARED_KEY_FILE="$TMP_ROOT/shared-key.txt"
rm -f "$SHARED_KEY_FILE"
WORKDIR2="$TMP_ROOT/work2"
mkdir -p "$WORKDIR2" && cp "$FAKE_AEGISGATE" "$WORKDIR2/aegisgate"

cd "$WORKDIR2" && \
    AEGISGATE_QUICKSTART_KEY_FILE="$SHARED_KEY_FILE" OPENAI_API_KEY=sk-test bash "$TARGET" >/dev/null 2>&1
KEY1=$(cat "$SHARED_KEY_FILE")

cd "$WORKDIR2" && \
    AEGISGATE_QUICKSTART_KEY_FILE="$SHARED_KEY_FILE" OPENAI_API_KEY=sk-test bash "$TARGET" >/dev/null 2>&1
KEY2=$(cat "$SHARED_KEY_FILE")
cd - >/dev/null

if [[ "$KEY1" == "$KEY2" ]] && [[ -n "$KEY1" ]]; then
    pass "T6 (key reused on restart: $KEY1)"
else
    fail "T6 (key changed: '$KEY1' vs '$KEY2')"
fi

echo "==> T7: missing OPENAI_API_KEY produces warning but does NOT block startup"
# run_entrypoint with NO env args -> OPENAI_API_KEY naturally absent (env -i strips it)
out=$(run_entrypoint)
if [[ "$out" == *"OPENAI_API_KEY not set"* ]] && [[ "$out" == *"fake-aegisgate"* ]]; then
    pass "T7 (warning shown, startup continues)"
else
    fail "T7 (expected warning + startup, got out tail=${out: -500})"
fi

echo "==> T8 (SR1 reverse): no LAN IP literal in script"
if grep -qE '192\.168\.[0-9]+\.[0-9]+' "$TARGET"; then
    fail "T8 (SR1 violated: hardcoded IP)"
else
    pass "T8 (no IP literal)"
fi

echo ""
echo "[SUMMARY] PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]]
