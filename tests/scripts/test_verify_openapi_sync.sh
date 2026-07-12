#!/usr/bin/env bash
# tests/scripts/test_verify_openapi_sync.sh
#
# Self-test for scripts/verify-openapi-sync.sh. Two scenarios:
#
#   1. Happy path — the real api/control-plane/openapi/control-plane-v1.yaml must
#      verify OK against the real
#      api/control-plane/proto/control_plane/v1/control_plane.proto.
#
#   2. Negative path — feed verify-openapi-sync.sh a yaml with a field
#      removed; it MUST fail.
#
# Usage:    bash tests/scripts/test_verify_openapi_sync.sh
# Exit:     0 = PASS, 1 = FAIL
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"

VERIFY_SH="scripts/verify-openapi-sync.sh"
REAL_YAML="api/control-plane/openapi/control-plane-v1.yaml"

fail=0
pass=0

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { pass=$((pass + 1)); printf '  OK: %s\n' "$*"; }
bad()  { fail=$((fail + 1)); printf '  FAIL: %s\n' "$*" >&2; }

step "precondition: verify script exists and is executable"
if [[ -x "$VERIFY_SH" ]]; then
    ok "$VERIFY_SH present + +x"
else
    bad "missing or non-executable: $VERIFY_SH"
    exit 1
fi

step "precondition: real openapi yaml exists"
if [[ -f "$REAL_YAML" ]]; then
    ok "$REAL_YAML present"
else
    bad "missing: $REAL_YAML"
    exit 1
fi

step "scenario 1: happy path (real yaml against real proto)"
if out=$(bash "$VERIFY_SH" 2>&1); then
    ok "verify passed (${out##*$'\n'})"
else
    bad "verify should have passed; got: $out"
fi

step "scenario 2: negative path (remove 'submitter_comment' field)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Drop every line that declares submitter_comment (the field appears as
# "submitter_comment:" in the OpenAPI components schema).
grep -v '^\s*submitter_comment\s*:' "$REAL_YAML" > "$tmp/broken.yaml"
if [[ "$(wc -l < "$REAL_YAML")" -le "$(wc -l < "$tmp/broken.yaml")" ]]; then
    bad "precondition: expected broken.yaml to be strictly smaller"
fi
if OPENAPI_FILE="$tmp/broken.yaml" bash "$VERIFY_SH" >/dev/null 2>&1; then
    bad "verify should have rejected yaml missing 'submitter_comment'"
else
    ok "verify correctly rejected yaml missing 'submitter_comment'"
fi

step "scenario 3: injection safety — yaml path containing whitespace"
# Ensure OPENAPI_FILE env handles spaces in paths.
tmp2="$tmp/dir with spaces"
mkdir -p "$tmp2"
cp "$REAL_YAML" "$tmp2/copy.yaml"
if OPENAPI_FILE="$tmp2/copy.yaml" bash "$VERIFY_SH" >/dev/null 2>&1; then
    ok "verify tolerates whitespace in OPENAPI_FILE path"
else
    bad "verify failed on whitespace-containing path"
fi

printf '\n=== summary: %d passed, %d failed ===\n' "$pass" "$fail"
exit "$fail"
