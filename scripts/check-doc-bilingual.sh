#!/usr/bin/env bash
# Bilingual Markdown symmetry checker.
#
# Verifies that English (foo.md) and Chinese (foo_zh.md) pairs of
# Markdown documents have matching H2 and H3 section counts. Used by
# /van workflow's environment checklist (.cursor/rules/workflow/van.md, C8).
#
# Usage:
#   scripts/check-doc-bilingual.sh EN_FILE ZH_FILE [EN_FILE ZH_FILE ...]
#   scripts/check-doc-bilingual.sh --pairs PAIRS_FILE      # one "en zh" per line
#   scripts/check-doc-bilingual.sh --auto-discover DIR     # auto-pair *.md <-> *_zh.md
#   scripts/check-doc-bilingual.sh --help
#
# Flags:
#   --also-h4        Also require H4 section counts to match (default: off).
#   --pairs FILE     Read pairs from a file ("en_path zh_path" per line,
#                    blank lines and `#`-comments allowed).
#   --auto-discover DIR
#                    For each `*_zh.md` under DIR, pair it with the matching
#                    `*.md` (strip `_zh` suffix). Skip orphan files with [INFO].
#   --quiet          Only print [MISMATCH] lines and final summary.
#
# Exit codes (per van.md C8 contract):
#   0  PASS    all pairs symmetric
#   1  WARN    one or more mismatched pairs
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
log_pass()     { printf '%b[PASS]%b      %s\n' "$GREEN" "$NC" "$*"; }
log_mismatch() { printf '%b[MISMATCH]%b  %s\n' "$RED" "$NC" "$*" >&2; }
log_info()     { printf '[INFO]      %s\n' "$*"; }
log_fail()     { printf '%b[FAIL]%b      %s\n' "$RED" "$NC" "$*" >&2; }

show_help() { sed -n '2,29p' "$0" | sed 's|^# \?||'; }

# --- Parse arguments ---
ALSO_H4=0
QUIET=0
PAIRS_FILE=""
AUTO_DIR=""
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h) show_help; exit 0 ;;
        --also-h4) ALSO_H4=1; shift ;;
        --quiet) QUIET=1; shift ;;
        --pairs)
            [[ -n "${2:-}" ]] || { log_fail "--pairs needs FILE"; exit 1; }
            PAIRS_FILE="$2"; shift 2 ;;
        --auto-discover)
            [[ -n "${2:-}" ]] || { log_fail "--auto-discover needs DIR"; exit 1; }
            AUTO_DIR="$2"; shift 2 ;;
        --) shift; while [[ $# -gt 0 ]]; do ARGS+=("$1"); shift; done ;;
        -*) log_fail "unknown flag: $1 (use --help)"; exit 1 ;;
        *)  ARGS+=("$1"); shift ;;
    esac
done

# --- Build pair list ---
declare -a EN_LIST=()
declare -a ZH_LIST=()

# From positional args (even count: en1 zh1 en2 zh2 ...)
if [[ ${#ARGS[@]} -gt 0 ]]; then
    if (( ${#ARGS[@]} % 2 != 0 )); then
        log_fail "positional args must be even count (en zh pairs); got ${#ARGS[@]}"
        exit 1
    fi
    for ((i=0; i<${#ARGS[@]}; i+=2)); do
        EN_LIST+=("${ARGS[i]}")
        ZH_LIST+=("${ARGS[i+1]}")
    done
fi

# From --pairs file
if [[ -n "$PAIRS_FILE" ]]; then
    [[ -f "$PAIRS_FILE" ]] || { log_fail "--pairs file not found: $PAIRS_FILE"; exit 1; }
    while IFS= read -r line; do
        # Strip comments and blank lines
        line="${line%%#*}"
        [[ -z "${line// }" ]] && continue
        # shellcheck disable=SC2206
        parts=( $line )
        if [[ ${#parts[@]} -ne 2 ]]; then
            log_fail "bad pair line (need 2 paths): $line"
            exit 1
        fi
        EN_LIST+=("${parts[0]}")
        ZH_LIST+=("${parts[1]}")
    done < "$PAIRS_FILE"
fi

# From --auto-discover
if [[ -n "$AUTO_DIR" ]]; then
    [[ -d "$AUTO_DIR" ]] || { log_fail "--auto-discover dir not found: $AUTO_DIR"; exit 1; }
    while IFS= read -r -d '' zh; do
        # Derive en path by stripping `_zh` before `.md`
        en="${zh%_zh.md}.md"
        if [[ -f "$en" ]]; then
            EN_LIST+=("$en")
            ZH_LIST+=("$zh")
        else
            log_info "orphan zh file (no en counterpart): $zh"
        fi
    done < <(find "$AUTO_DIR" -type f -name '*_zh.md' -print0)
fi

if [[ ${#EN_LIST[@]} -eq 0 ]]; then
    log_fail "no pairs provided (use positional args, --pairs, or --auto-discover)"
    exit 1
fi

# --- Validate each pair ---
total=${#EN_LIST[@]}
symmetric=0
mismatched=0

for ((i=0; i<total; i++)); do
    en="${EN_LIST[i]}"
    zh="${ZH_LIST[i]}"
    if [[ ! -f "$en" ]]; then
        log_fail "missing en file: $en"
        mismatched=$((mismatched+1))
        continue
    fi
    if [[ ! -f "$zh" ]]; then
        log_fail "missing zh file: $zh"
        mismatched=$((mismatched+1))
        continue
    fi
    en_h2=$(grep -c '^## ' "$en" || true)
    zh_h2=$(grep -c '^## ' "$zh" || true)
    en_h3=$(grep -c '^### ' "$en" || true)
    zh_h3=$(grep -c '^### ' "$zh" || true)
    ok=1
    [[ "$en_h2" == "$zh_h2" ]] || ok=0
    [[ "$en_h3" == "$zh_h3" ]] || ok=0
    if [[ $ALSO_H4 -eq 1 ]]; then
        en_h4=$(grep -c '^#### ' "$en" || true)
        zh_h4=$(grep -c '^#### ' "$zh" || true)
        h4_part=" H4=$en_h4|$zh_h4"
        [[ "$en_h4" == "$zh_h4" ]] || ok=0
    else
        h4_part=""
    fi
    summary="$en (H2=$en_h2 H3=$en_h3$h4_part) vs $(basename "$zh") (H2=$zh_h2 H3=$zh_h3${h4_part:+ H4=$zh_h4})"
    if [[ $ok -eq 1 ]]; then
        symmetric=$((symmetric+1))
        [[ $QUIET -eq 0 ]] && log_pass "$summary"
    else
        mismatched=$((mismatched+1))
        log_mismatch "$summary"
        hint=""
        (( en_h2 > zh_h2 )) && hint="$hint  en has $((en_h2-zh_h2)) more H2"
        (( zh_h2 > en_h2 )) && hint="$hint  zh has $((zh_h2-en_h2)) more H2"
        (( en_h3 > zh_h3 )) && hint="$hint  en has $((en_h3-zh_h3)) more H3"
        (( zh_h3 > en_h3 )) && hint="$hint  zh has $((zh_h3-en_h3)) more H3"
        [[ -n "$hint" ]] && printf '            hint:%s\n' "$hint" >&2
    fi
done

log_info "$symmetric/$total pairs symmetric"
[[ $mismatched -eq 0 ]] && exit 0 || exit 1
