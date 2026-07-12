#!/usr/bin/env bash
# Feature-list config-key parity checker.
#
# Guards against config-key drift between the public docs and the running code —
# the exact class of "false closure" that S2 / S12 represented (docs declaring a
# config key the C++ code never reads, e.g. `features.deployment.mode` while the
# code reads top-level `deployment.mode`, or a guide claiming there is "no
# top-level `token_optimization.*` root" while the code reads exactly that).
#
# Truth source (dynamic): the top-level config roots the code actually reads in
# src/core/config.cpp — both `Config::safeGet("<root>", ...)` and direct
# `root_["<root>"]` node access. A documented config key whose top-level root is
# neither read by the code nor in the curated EXTRA_VALID_ROOTS allowlist (and is
# not in DENY_ROOTS) is reported as drift.
#
# Two checks:
#   CHECK 1  feature-list config-key column  — the key column of capability
#            tables in docs/feature-list.md[_zh].md (the column right after the
#            status marker ⚪/🔵/🟢/🟡/🔴).
#   CHECK 2  config-guide YAML roots         — top-level keys of ```yaml fenced
#            blocks in curated config-reference guides (cost-optimization.md[_zh]).
#
# Usage:
#   scripts/check-feature-list-parity.sh            # check tracked docs
#   scripts/check-feature-list-parity.sh --list     # print resolved code roots
#   scripts/check-feature-list-parity.sh --help
#
# Exit codes:
#   0  PASS   no drift
#   1  FAIL   one or more documented config roots not honored by the code
#   2  ERROR  bad usage / missing inputs
#
# Security requirements:
#   SR1  No literal LAN IP in this file.
#   SR5  Read-only (never mutates the repo).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG_CPP="$ROOT_DIR/src/core/config.cpp"
FEATURE_LISTS=(
    "$ROOT_DIR/docs/feature-list.md"
    "$ROOT_DIR/docs/feature-list_zh.md"
)
GUIDE_YAML_DOCS=(
    "$ROOT_DIR/docs/guides/cost-optimization.md"
    "$ROOT_DIR/docs/guides/cost-optimization_zh.md"
)

# Config roots documented as namespaces but honored via mechanisms the parser
# above cannot see (provider manifests / FeatureGate license gating) or referring
# to runtime data namespaces rather than YAML roots.
# Keep this list minimal and review it whenever config plumbing changes.
EXTRA_VALID_ROOTS=(
    providers    # provider manifests: api/providers/definitions/*.yaml
    feedback_bus # feedback bus subsystem
    rbac         # RBAC is a FeatureGate/license feature; rbac.* is intentionally
                 # documented as gated (see I9), not a plain config root
    cost         # CostTracker runtime cost-record namespace (not a yaml root)
    tenants      # per-tenant runtime data namespace (not a yaml root)
)

# Roots the code DOES touch but under which nested config paths are still drift.
# `features.*` is a boolean feature-toggle map (root_["features"][name] -> bool),
# so a nested config path like `features.deployment.mode` is a dead key — the
# real setting lives at a top-level root (`deployment.mode`). This is exactly the
# S12 false-closure pattern, so `features` must never count as a valid config
# root for documented dotted keys.
DENY_ROOTS=(
    features
)

if [[ -t 1 ]]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; NC=''
fi
log_pass() { printf '%b[PASS]%b   %s\n' "$GREEN" "$NC" "$*"; }
log_fail() { printf '%b[FAIL]%b   %s\n' "$RED" "$NC" "$*" >&2; }
log_info() { printf '[INFO]   %s\n' "$*"; }
log_drift(){ printf '%b[DRIFT]%b  %s\n' "$RED" "$NC" "$*" >&2; }

show_help() { sed -n '2,33p' "$0" | sed 's|^# \?||'; }

# --- Resolve the authoritative code roots -----------------------------------
[[ -f "$CONFIG_CPP" ]] || { log_fail "config source not found: $CONFIG_CPP"; exit 2; }
mapfile -t CODE_ROOTS < <(
    {
        # 1. safeGet<T>("root", ...) — first string arg is the top-level root
        grep -oE 'safeGet<[^>]*>\("[a-zA-Z_][a-zA-Z0-9_]*"' "$CONFIG_CPP" \
            | sed -E 's/.*\("([^"]+)".*/\1/'
        # 2. root_["root"] — direct YAML node access
        grep -oE 'root_\["[a-zA-Z_][a-zA-Z0-9_]*"\]' "$CONFIG_CPP" \
            | sed -E 's/root_\["([^"]+)"\]/\1/'
    } | sort -u
)
if [[ ${#CODE_ROOTS[@]} -eq 0 ]]; then
    log_fail "no config roots parsed from $CONFIG_CPP (parser broken?)"
    exit 2
fi

case "${1:-}" in
    --help|-h) show_help; exit 0 ;;
    --list)
        printf 'code roots (%d): %s\n' "${#CODE_ROOTS[@]}" "${CODE_ROOTS[*]}"
        printf 'extra valid (%d): %s\n' "${#EXTRA_VALID_ROOTS[@]}" "${EXTRA_VALID_ROOTS[*]}"
        printf 'deny roots (%d): %s\n' "${#DENY_ROOTS[@]}" "${DENY_ROOTS[*]}"
        exit 0 ;;
    "") ;;
    *) log_fail "unknown argument: $1 (use --help)"; exit 2 ;;
esac

is_valid_root() {
    local r="$1" v
    # DENY_ROOTS win even if the code touches them (see comment above).
    for v in "${DENY_ROOTS[@]}"; do
        [[ "$r" == "$v" ]] && return 1
    done
    for v in "${CODE_ROOTS[@]}" "${EXTRA_VALID_ROOTS[@]}"; do
        [[ "$r" == "$v" ]] && return 0
    done
    return 1
}

# Extract candidate config-key tokens (dotted, lowercase-rooted, no slash) from a
# text fragment and emit their top-level roots, one per line.
roots_from_fragment() {
    local frag="$1"
    grep -oE '`[^`]+`' <<<"$frag" 2>/dev/null | tr -d '`' | while IFS= read -r tok; do
        # a config key looks like `root.sub...`; skip endpoints/paths (contain /),
        # env vars (UPPERCASE), and anything without a dot.
        [[ "$tok" == */* ]] && continue
        [[ "$tok" =~ ^[a-z][a-z0-9_]*\. ]] || continue
        sed -E 's/^([a-z0-9_]+).*/\1/' <<<"$tok"
    done
}

fail=0

# --- CHECK 1: feature-list config-key column --------------------------------
for f in "${FEATURE_LISTS[@]}"; do
    [[ -f "$f" ]] || { log_fail "missing feature list: $f"; fail=1; continue; }
    # Only capability-table rows: field 4 (after leading empty split on |) holds
    # a status marker; field 5 is the config-key column.
    while IFS= read -r keycol; do
        [[ -z "$keycol" ]] && continue
        while IFS= read -r root; do
            [[ -z "$root" ]] && continue
            if ! is_valid_root "$root"; then
                log_drift "$(basename "$f"): config key root '$root' not read by code (config.cpp) nor allowlisted"
                fail=1
            fi
        done < <(roots_from_fragment "$keycol")
    done < <(
        awk -F'|' '
            { status=$4; key=$5 }
            status ~ /⚪|🔵|🟢|🟡|🔴/ { print key }
        ' "$f"
    )
done

# --- CHECK 2: config-guide YAML top-level roots -----------------------------
for f in "${GUIDE_YAML_DOCS[@]}"; do
    [[ -f "$f" ]] || { log_fail "missing guide: $f"; fail=1; continue; }
    while IFS= read -r root; do
        [[ -z "$root" ]] && continue
        if ! is_valid_root "$root"; then
            log_drift "$(basename "$f"): YAML root '$root' not read by code (config.cpp) nor allowlisted"
            fail=1
        fi
    done < <(
        awk '
            /^```[ ]*ya?ml[ ]*$/ { in_yaml=1; next }
            /^```/               { in_yaml=0; next }
            in_yaml && /^[a-z][a-z0-9_]*:/ {
                sub(/:.*/, "", $0); print $0
            }
        ' "$f" | sort -u
    )
done

if [[ $fail -eq 0 ]]; then
    log_pass "config-key parity OK (${#CODE_ROOTS[@]} code roots + ${#EXTRA_VALID_ROOTS[@]} allowlisted)"
    exit 0
fi
log_fail "config-key drift detected — align the doc to the key the code actually reads (grep src/core/config.cpp for safeGet), or add a justified root to EXTRA_VALID_ROOTS"
exit 1
