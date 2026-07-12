#!/usr/bin/env bash
# Configurable wording-compliance grep scanner for Markdown documents.
#
# Used by /van workflow's environment checklist
# (.cursor/rules/workflow/van.md, C9).
#
# Scans Markdown files for occurrences of banned wording (e.g. K2
# compliance-truthfulness wording: "compliant" / "certified" /
# "production-grade" / "enterprise-ready"). Lines matching an exempt
# pattern (e.g. disclaimer paragraphs that explicitly negate the wording)
# are excluded.
#
# Usage:
#   scripts/check-doc-wording.sh --banned WORDS [OPTIONS] FILE [FILE ...]
#   scripts/check-doc-wording.sh --help
#
# Required flags:
#   --banned WORDS           Comma-separated list of banned word stems
#                            (matched as `\bWORD\b` using grep -wE).
#                            Example: 'compliant,certified,production-grade'.
#
# Optional flags:
#   --exempt-pattern REGEX   grep -E pattern; matching lines are excluded
#                            from violations. Default:
#                            'disclaimer|不构成|aspirational|conformity|placeholder'.
#                            (Tip: include patterns like `"compliant"`
#                            for meta-discussion exclusions.)
#   --severity N             Exit code N when violations are found (default 1).
#                            Use --severity 2 for hard compliance gates.
#   --quiet                  Only print [WARN] lines and final summary.
#
# Exit codes (per van.md C9 contract):
#   0  PASS    no violations
#   1  WARN    >=1 violation (default; can be overridden with --severity)
#   2  ERROR   >=1 violation when --severity 2 is set
#
# Security requirements:
#   SR1  No literal LAN IP in this file.
#   SR5  Read-only.
set -euo pipefail

if [[ -t 1 ]]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; NC=''
fi
log_pass()  { printf '%b[PASS]%b  %s\n' "$GREEN" "$NC" "$*"; }
log_warn()  { printf '%b[WARN]%b  %s\n' "$YELLOW" "$NC" "$*" >&2; }
log_fail()  { printf '%b[FAIL]%b  %s\n' "$RED" "$NC" "$*" >&2; }
log_info()  { printf '[INFO]  %s\n' "$*"; }

show_help() { sed -n '2,36p' "$0" | sed 's|^# \?||'; }

# --- Defaults ---
BANNED=""
EXEMPT_PATTERN='disclaimer|不构成|aspirational|conformity|placeholder'
SEVERITY=1
QUIET=0
ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h) show_help; exit 0 ;;
        --banned)
            [[ -n "${2:-}" ]] || { log_fail "--banned needs argument"; exit 1; }
            BANNED="$2"; shift 2 ;;
        --exempt-pattern)
            [[ -n "${2:-}" ]] || { log_fail "--exempt-pattern needs argument"; exit 1; }
            EXEMPT_PATTERN="$2"; shift 2 ;;
        --severity)
            [[ "${2:-}" =~ ^[012]$ ]] || { log_fail "--severity must be 0, 1, or 2"; exit 1; }
            SEVERITY="$2"; shift 2 ;;
        --quiet) QUIET=1; shift ;;
        --) shift; while [[ $# -gt 0 ]]; do ARGS+=("$1"); shift; done ;;
        -*) log_fail "unknown flag: $1 (use --help)"; exit 1 ;;
        *)  ARGS+=("$1"); shift ;;
    esac
done

if [[ -z "$BANNED" ]]; then
    log_fail "--banned is required (use --help)"
    exit 1
fi

if [[ ${#ARGS[@]} -eq 0 ]]; then
    log_fail "no input file; pass at least one Markdown file"
    exit 1
fi

# --- Collect target *.md files (expand directories) ---
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
        exit 1
    fi
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    log_info "no Markdown files matched"
    exit 0
fi

# --- Build regex from --banned (comma-separated word stems) ---
# Use word-boundary alternation: \b(w1|w2|...)\b for grep -wE.
# grep -w already handles word boundaries; we just join with |.
banned_pattern=$(printf '%s' "$BANNED" | sed 's/,/|/g')

# --- Scan ---
violations=0
declare -a VIOLATION_REPORT=()
exempted=0

for f in "${TARGETS[@]}"; do
    # Compute the set of line numbers inside fenced code blocks (``` ... ```).
    # Lines inside fences are excluded from wording violations (banned words
    # appearing in example code / fixture descriptions are legitimate meta
    # discussion, e.g. `grep 'compliant'` in a tutorial).
    in_fence_lines=$(awk '
        BEGIN { in_fence=0 }
        /^[[:space:]]*```/ { in_fence = 1 - in_fence; print NR; next }
        in_fence==1 { print NR }
    ' "$f" | sort -un)

    # grep -wE: word-boundary alternation; -H: filename prefix; -n: line number
    # `|| true` because grep exits 1 when no match.
    raw=$(grep -wEnH "($banned_pattern)" "$f" 2>/dev/null || true)
    [[ -z "$raw" ]] && continue
    while IFS= read -r hit; do
        # Extract line number (format: file:lineno:content)
        lineno=$(echo "$hit" | cut -d: -f2)
        # Skip if line is inside a fenced code block
        if [[ -n "$in_fence_lines" ]] && echo "$in_fence_lines" | grep -qx "$lineno"; then
            exempted=$((exempted+1))
            continue
        fi
        # Apply exempt pattern (line-level)
        if [[ -n "$EXEMPT_PATTERN" ]] && echo "$hit" | grep -qE "$EXEMPT_PATTERN"; then
            exempted=$((exempted+1))
            continue
        fi
        # Also exempt if the matched word appears only inside inline code (`...`)
        # on this line — that's meta-discussion of the word itself.
        line_no_inline=$(echo "$hit" | sed 's/`[^`]*`//g')
        if ! echo "$line_no_inline" | grep -qwE "($banned_pattern)"; then
            exempted=$((exempted+1))
            continue
        fi
        violations=$((violations+1))
        # Extract the matched word for the report
        matched=$(echo "$hit" | grep -woE "($banned_pattern)" | head -1)
        # Format: file:line:matched_word
        prefix=$(echo "$hit" | cut -d: -f1-2)
        VIOLATION_REPORT+=("$prefix:$matched")
    done <<< "$raw"
done

# --- Report ---
if [[ $violations -eq 0 ]]; then
    log_pass "no violations in ${#TARGETS[@]} files (banned: $BANNED / exempted lines: $exempted)"
    exit 0
else
    log_warn "wording violations found:"
    for entry in "${VIOLATION_REPORT[@]}"; do
        printf '  %s\n' "$entry" >&2
    done
    log_info "$violations violations in ${#TARGETS[@]} files (banned: $BANNED / exempted: $exempted lines)"
    exit "$SEVERITY"
fi
