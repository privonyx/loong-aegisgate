#!/usr/bin/env bash
# 共享验收辅助库：计数、断言、HTTP 工具。被 run_community.sh / run_enterprise.sh 引用。
# 注意：不要 set -e —— 验收脚本需在单个用例失败后继续执行并汇总。
set -uo pipefail

: "${BASE:=http://127.0.0.1:8080}"
: "${KEY:=${AEGISGATE_API_KEY:-}}"
: "${ADMIN_KEY:=${AEGISGATE_ADMIN_KEY:-}}"
: "${MODEL:=deepseek-chat}"
: "${WITH_UPSTREAM:=0}"   # 1 = 允许真实调用 DeepSeek（产生费用）

if [[ -t 1 ]]; then
  C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_RST=$'\033[0m'
else
  C_GREEN=""; C_RED=""; C_YEL=""; C_DIM=""; C_RST=""
fi

PASS_N=0; FAIL_N=0; SKIP_N=0
FAILED_IDS=()

_log()  { printf '%s\n' "$*"; }
_pass() { PASS_N=$((PASS_N+1)); _log "  ${C_GREEN}PASS${C_RST} [$1] $2"; }
_fail() { FAIL_N=$((FAIL_N+1)); FAILED_IDS+=("$1"); _log "  ${C_RED}FAIL${C_RST} [$1] $2"; }
_skip() { SKIP_N=$((SKIP_N+1)); _log "  ${C_YEL}SKIP${C_RST} [$1] $2"; }

section() { _log ""; _log "${C_DIM}== $* ==${C_RST}"; }

# assert_eq <id> <desc> <expected> <actual>
assert_eq() {
  if [[ "$3" == "$4" ]]; then _pass "$1" "$2 (=$4)"; else _fail "$1" "$2 (期望 $3, 实际 $4)"; fi
}
# assert_contains <id> <desc> <needle> <haystack>
assert_contains() {
  if [[ "$4" == *"$3"* ]]; then _pass "$1" "$2"; else _fail "$1" "$2 (未找到 '$3')"; fi
}
# assert_not_contains <id> <desc> <needle> <haystack>
assert_not_contains() {
  if [[ "$4" != *"$3"* ]]; then _pass "$1" "$2"; else _fail "$1" "$2 (不应出现 '$3')"; fi
}

# http_code <method> <path> [extra curl args...]  -> 仅打印 HTTP 状态码
http_code() {
  local method="$1" path="$2"; shift 2
  curl -s -o /dev/null -w '%{http_code}' -X "$method" "$BASE$path" "$@"
}
# http_body <method> <path> [extra curl args...]  -> 打印响应体
http_body() {
  local method="$1" path="$2"; shift 2
  curl -s -X "$method" "$BASE$path" "$@"
}

# chat_body <prompt>  -> 生成 chat completions 请求体
chat_body() {
  printf '{"model":"%s","messages":[{"role":"user","content":%s}]}' \
    "$MODEL" "$(json_str "$1")"
}
json_str() { python3 -c 'import json,sys;print(json.dumps(sys.argv[1],ensure_ascii=False))' "$1"; }

require_upstream() {  # 用例 id, desc；返回 0 可继续，1 应跳过
  if [[ "$WITH_UPSTREAM" != "1" ]]; then
    _skip "$1" "$2 (需 --with-upstream 真实调用 DeepSeek)"
    return 1
  fi
  return 0
}

summary() {
  _log ""
  _log "================ 验收汇总 ================"
  _log "  ${C_GREEN}PASS=$PASS_N${C_RST}  ${C_RED}FAIL=$FAIL_N${C_RST}  ${C_YEL}SKIP=$SKIP_N${C_RST}"
  if (( FAIL_N > 0 )); then _log "  失败用例: ${FAILED_IDS[*]}"; fi
  _log "=========================================="
  (( FAIL_N == 0 ))
}
