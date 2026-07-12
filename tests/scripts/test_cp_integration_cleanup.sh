#!/usr/bin/env bash
# tests/scripts/test_cp_integration_cleanup.sh
#
# Verifies that scripts/test-control-plane-local.sh cleans up its tmpdir
# and its forked subprocesses even when the parent is killed mid-flight.
#
# The integration script exposes an undocumented dev hook so this test can
# induce a long-lived state:
#   AEGISGATE_IT_PAUSE_AFTER_BOOT=<seconds>  -- after forking both binaries
#                                               and before running aegisctl,
#                                               sleep N seconds. The test
#                                               kills the parent during this
#                                               window.
#
# Exit:     0 = PASS, 1 = FAIL
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$REPO/scripts/test-control-plane-local.sh"

if [[ ! -x "$SCRIPT" ]]; then
    printf 'FAIL: integration script not found or not executable: %s\n' "$SCRIPT" >&2
    exit 1
fi

have_binary() {
    local name="$1"
    for candidate in \
        "$REPO/build-cp-on/$name" \
        "$REPO/build-cp-on/src/$name" \
        "$REPO/build-cp-on/src/control_plane/$name"; do
        [[ -x "$candidate" ]] && return 0
    done
    return 1
}

if ! have_binary aegisgate-control-plane || ! have_binary aegisctl; then
    echo "SKIP: build-cp-on binaries not present — run the ON-path build first" >&2
    exit 0
fi

fail=0

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  OK: %s\n' "$*"; }
bad()  { fail=$((fail + 1)); printf '  FAIL: %s\n' "$*" >&2; }

# -----------------------------------------------------------------------------
# Scenario 1: parent killed with SIGTERM during AEGISGATE_IT_PAUSE_AFTER_BOOT.
#
# We deliberately send SIGTERM rather than SIGINT. When a bash script is
# launched via `setsid` with no controlling terminal, SIGINT is silently
# dropped by the kernel/bash even when a `trap ... INT` is registered —
# SIGINT is only delivered by the TTY driver. SIGTERM goes through the
# same trap path in the integration script and exercises the same cleanup
# code. Ctrl-C in an interactive terminal (the actual operator path) is
# verified manually.
# -----------------------------------------------------------------------------
step "scenario 1: SIGTERM to parent during boot pause — expect full cleanup"

marker_dir="$(mktemp -d -t aegisgate-it-marker.XXXXXX)"
export AEGISGATE_IT_MARKER_DIR="$marker_dir"
export AEGISGATE_IT_PAUSE_AFTER_BOOT=20

# Launch integration script in its own process group so we can signal the
# whole group later and observe the trap.
setsid bash "$SCRIPT" >"$marker_dir/stdout.log" 2>"$marker_dir/stderr.log" &
parent_pid=$!

# Wait until the script logs "boot-pause start" (emitted right before the
# pause sleep). Timeout 20s — if the boot itself fails we still want to
# exercise cleanup.
for _ in $(seq 1 40); do
    grep -q 'boot-pause start' "$marker_dir/stderr.log" 2>/dev/null && break
    sleep 0.5
done

if ! grep -q 'boot-pause start' "$marker_dir/stderr.log" 2>/dev/null; then
    # Boot failed. We can still confirm cleanup: wait for parent to exit,
    # then check that no tmp dir or orphan processes remain.
    wait "$parent_pid" 2>/dev/null || true
    printf '  (boot never reached pause; stderr tail:)\n' >&2
    tail -20 "$marker_dir/stderr.log" >&2 || true
fi

# Send SIGTERM to the process group.
kill -TERM -- "-$parent_pid" 2>/dev/null || true

# Give the trap time to fire (the script's cleanup kills children + rm -rf).
sleep 3

# Parent should be gone.
if kill -0 "$parent_pid" 2>/dev/null; then
    bad "parent ($parent_pid) still running 3s after SIGTERM"
    kill -KILL -- "-$parent_pid" 2>/dev/null || true
else
    ok "parent exited after SIGTERM"
fi

# No orphan aegisgate-control-plane / aegisgate / aegisctl processes.
# pgrep returns 1 when there are zero matches; that's the happy case here,
# so mask its exit and rely solely on the row count.
orphans=$(pgrep -f 'aegisgate-control-plane|aegisctl|build-cp-on/aegisgate' \
          2>/dev/null | wc -l || true)
if [[ "$orphans" -ne 0 ]]; then
    bad "$orphans orphan process(es) left behind"
    pgrep -af 'aegisgate-control-plane|aegisctl|build-cp-on/aegisgate' >&2 || true
    pkill -KILL -f 'aegisgate-control-plane|aegisctl|build-cp-on/aegisgate' \
        2>/dev/null || true
else
    ok "no orphan processes"
fi

# Every script-owned /tmp/aegisgate-it.XXXXXX must have been removed by the
# script's trap. We can't observe them directly (random suffixes), but the
# script writes the tmp path to $AEGISGATE_IT_MARKER_DIR/tmp_path for tests.
if [[ -f "$marker_dir/tmp_path" ]]; then
    tmp_path="$(cat "$marker_dir/tmp_path")"
    if [[ -n "$tmp_path" && -e "$tmp_path" ]]; then
        bad "script-owned tmpdir still present: $tmp_path"
        rm -rf "$tmp_path"
    else
        ok "script tmpdir removed"
    fi
else
    ok "script did not emit tmp_path marker (early-exit path); ok"
fi

rm -rf "$marker_dir"

# -----------------------------------------------------------------------------
printf '\n=== summary: %d failure(s) ===\n' "$fail"
exit "$fail"
