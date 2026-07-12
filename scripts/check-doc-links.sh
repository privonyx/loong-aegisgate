#!/usr/bin/env bash
# Generic dead-link scanner for Markdown documents.
#
# Used by /van workflow's environment checklist
# (.cursor/rules/workflow/van.md, C7).
#
# Scans Markdown files for relative-path links of the form
# `[text](path.md)` or `[text](./path.md)` and verifies that each
# linked file exists. External URLs (http*://) are ignored by default.
# Anchor-only links (#section) are also ignored.
#
# Usage:
#   scripts/check-doc-links.sh FILE [FILE ...]      # check specific files
#   scripts/check-doc-links.sh DIR  [DIR  ...]      # recursively check *.md under DIR
#   scripts/check-doc-links.sh --help               # show this help
#
# Flags:
#   --exclude PATTERN   Skip files whose path matches this grep -E pattern.
#                       Repeatable. Default excludes none.
#   --quiet             Only print [FAIL] lines and final summary.
#
# Exit codes (per van.md C7 contract):
#   0  PASS    all local links resolved
#   2  ERROR   one or more dead links found
#
# Security requirements:
#   SR1  No literal LAN IP in this file.
#   SR5  Read-only (no writes outside stdout/stderr).
set -euo pipefail

# --- Color helpers (style: scripts/check-windows-proxy.sh) ---
if [[ -t 1 ]]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; NC=''
fi
log_pass()  { printf '%b[PASS]%b  %s\n' "$GREEN" "$NC" "$*"; }
log_warn()  { printf '%b[WARN]%b  %s\n' "$YELLOW" "$NC" "$*"; }
log_fail()  { printf '%b[FAIL]%b  %s\n' "$RED" "$NC" "$*" >&2; }
log_info()  { printf '[INFO]  %s\n' "$*"; }

show_help() {
    sed -n '2,29p' "$0" | sed 's|^# \?||'
}

# --- Argument parsing ---
EXCLUDES=()
QUIET=0
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h) show_help; exit 0 ;;
        --exclude)
            [[ -n "${2:-}" ]] || { log_fail "--exclude needs an argument"; exit 2; }
            EXCLUDES+=("$2"); shift 2 ;;
        --quiet) QUIET=1; shift ;;
        --) shift; while [[ $# -gt 0 ]]; do ARGS+=("$1"); shift; done ;;
        -*) log_fail "unknown flag: $1 (use --help)"; exit 2 ;;
        *)  ARGS+=("$1"); shift ;;
    esac
done

if [[ ${#ARGS[@]} -eq 0 ]]; then
    log_fail "no input file or directory; pass at least one (use --help)"
    exit 2
fi

# --- Collect target *.md files ---
TARGETS=()
for arg in "${ARGS[@]}"; do
    if [[ -f "$arg" ]]; then
        TARGETS+=("$arg")
    elif [[ -d "$arg" ]]; then
        while IFS= read -r -d '' f; do
            TARGETS+=("$f")
        done < <(find "$arg" -type f -name '*.md' -print0)
    else
        log_fail "input not found: $arg"
        exit 2
    fi
done

# Apply --exclude filters
if [[ ${#EXCLUDES[@]} -gt 0 ]]; then
    FILTERED=()
    for f in "${TARGETS[@]}"; do
        skip=0
        for pat in "${EXCLUDES[@]}"; do
            if [[ "$f" =~ $pat ]]; then skip=1; break; fi
        done
        [[ $skip -eq 0 ]] && FILTERED+=("$f")
    done
    TARGETS=("${FILTERED[@]}")
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    log_warn "no Markdown files matched (after --exclude filters)"
    exit 0
fi

# --- Link extraction + resolution ---
#
# Extraction strategy:
#   1. grep -oE pattern matches  `](path)` segments where path does not start
#      with `h` (excludes http://, https://) and does not start with `#`
#      (excludes pure anchors).
#   2. Strip surrounding `](` and `)`.
#   3. Strip trailing `#anchor` (we only check file-level resolution).
#   4. Skip empty paths and paths starting with `mailto:` / `tel:`.
#   5. Resolve relative to the directory containing the file.

total_links=0
dead_links=0
declare -a DEAD_REPORT=()

for f in "${TARGETS[@]}"; do
    dir=$(dirname "$f")
    # Strip fenced code blocks (``` ... ```) and inline code (`...`) before
    # extracting links — Markdown example links inside code blocks must
    # NOT be treated as real links. We use awk to drop fenced regions, then
    # sed to strip inline backtick segments per line.
    # Note: Markdown allows up to 3 leading spaces before ``` and arbitrary
    # indentation inside nested list items, so we match `^[[:space:]]*````.
    stripped=$(awk '
        BEGIN { in_fence=0 }
        /^[[:space:]]*```/ { in_fence = 1 - in_fence; next }
        in_fence==0 { print }
    ' "$f" | sed 's/`[^`]*`//g')
    # Extract candidate links; allow paths starting with any non-h/non-# char.
    # Use a temp var to survive set -e if grep finds nothing.
    raw=$(echo "$stripped" | grep -oE '\]\([^)h#][^)]*\)|\]\(h[^t][^)]*\)' 2>/dev/null || true)
    [[ -z "$raw" ]] && continue
    while IFS= read -r match; do
        # Strip `](` prefix and `)` suffix.
        link="${match#](}"
        link="${link%)}"
        # Strip trailing anchor.
        link="${link%%#*}"
        # Skip empty / scheme-prefixed.
        [[ -z "$link" ]] && continue
        case "$link" in
            mailto:*|tel:*|http://*|https://*|ftp://*) continue ;;
        esac
        total_links=$((total_links+1))
        # Resolve relative to the source file's directory.
        target="$dir/$link"
        if [[ ! -e "$target" ]]; then
            dead_links=$((dead_links+1))
            DEAD_REPORT+=("$f -> $link")
        fi
    done <<< "$raw"
done

# --- Report ---
if [[ $dead_links -eq 0 ]]; then
    log_pass "all $total_links links resolved in ${#TARGETS[@]} files"
    exit 0
else
    log_fail "dead links found:"
    for entry in "${DEAD_REPORT[@]}"; do
        printf '  %s\n' "$entry" >&2
    done
    log_info "$dead_links dead links in ${#TARGETS[@]} files scanned (out of $total_links total local links)"
    exit 2
fi
