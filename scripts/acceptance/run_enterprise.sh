#!/usr/bin/env bash
# AegisGate 企业版自动化验收（HTTP 可测子集）。
#
# 前置:
#   1) 已用 gen_license.py 生成 license 并在 aegisgate.yaml 设置
#      edition: enterprise / license_file / rbac.enabled: true，已重启生效。
#   2) 提供管理员凭证用于 Admin 面板用例（任选其一）:
#        ADMIN_USER / ADMIN_PASS   -> 走 /admin/api/auth/login
#      或 ADMIN_SESSION             -> 已有的 session cookie / bearer
#
# 用法:
#   BASE=... KEY=... ADMIN_KEY=... ADMIN_USER=admin ADMIN_PASS=... ./run_enterprise.sh [--with-upstream]
#
# 非 HTTP 可测的能力（插件/Agent/RAG/集群/ControlPlane）在文档中以手工用例覆盖，
# 此脚本对其标记 SKIP 并给出提示。

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$HERE/lib.sh"

[[ "${1:-}" == "--with-upstream" ]] && WITH_UPSTREAM=1
: "${ADMIN_USER:=}"; : "${ADMIN_PASS:=}"; : "${ADMIN_SESSION:=}"

if [[ -z "$KEY" ]]; then echo "错误: 需设置 KEY 或 AEGISGATE_API_KEY" >&2; exit 2; fi
AUTH=(-H "Authorization: Bearer $KEY")
JSON=(-H "Content-Type: application/json")

_log "AegisGate 企业版验收  BASE=$BASE  WITH_UPSTREAM=$WITH_UPSTREAM"

# ---------- 2.1 License / 企业态确认 ----------
section "License / 企业态"
# 通过 metrics 或 admin/me 间接确认企业态；这里探测 RBAC 端点是否存在
me_code=$(http_code GET /admin/api/me "${AUTH[@]}")
if [[ "$me_code" != "404" ]]; then _pass EE-01 "企业 Admin 路由已注册 (/admin/api/me -> $me_code)"
else _fail EE-01 "企业 Admin 路由缺失（license 可能未生效或社区版）"; fi

# ---------- 2.3 Admin 登录 ----------
section "Admin 管理面"
SESSION_HDR=()
if [[ -n "$ADMIN_SESSION" ]]; then
  SESSION_HDR=(-H "Authorization: Bearer $ADMIN_SESSION")
  _pass EE-12 "使用预置 ADMIN_SESSION"
elif [[ -n "$ADMIN_USER" && -n "$ADMIN_PASS" ]]; then
  login=$(http_body POST /admin/api/auth/login "${JSON[@]}" \
    -d "{\"username\":$(json_str "$ADMIN_USER"),\"password\":$(json_str "$ADMIN_PASS")}")
  tok=$(printf '%s' "$login" | python3 -c 'import sys,json;
try:
  d=json.load(sys.stdin); print(d.get("token") or d.get("access_token") or "")
except Exception: print("")')
  if [[ -n "$tok" ]]; then SESSION_HDR=(-H "Authorization: Bearer $tok"); _pass EE-12 "管理员登录成功"
  else _fail EE-12 "登录未返回 token: $login"; fi
else
  _skip EE-12 "未提供 ADMIN_USER/ADMIN_PASS 或 ADMIN_SESSION"
fi

run_admin_get() { # id desc path
  if [[ ${#SESSION_HDR[@]} -eq 0 ]]; then _skip "$1" "$2 (无管理员会话)"; return; fi
  local c; c=$(http_code GET "$3" "${SESSION_HDR[@]}")
  if [[ "$c" == "200" ]]; then _pass "$1" "$2 ($3 -> 200)"
  else _fail "$1" "$2 ($3 -> $c)"; fi
}
run_admin_get EE-13 "租户列表" /admin/api/tenants
run_admin_get EE-14 "用户列表" /admin/api/users
run_admin_get EE-15 "API Key 列表" /admin/api/keys
run_admin_get EE-16 "Dashboard 概要" /admin/api/dashboard/summary
run_admin_get EE-16 "Savings 概要" /admin/api/savings/summary
run_admin_get EE-17 "审计查询" /admin/api/audits
run_admin_get EE-17 "成本查询" /admin/api/costs

# ---------- 2.2 RBAC 多租户隔离（需两个租户 key）----------
section "RBAC / 多租户隔离"
: "${TENANT_A_KEY:=}"; : "${TENANT_B_KEY:=}"
if [[ -n "$TENANT_A_KEY" && -n "$TENANT_B_KEY" ]]; then
  ca=$(http_code GET /v1/models -H "Authorization: Bearer $TENANT_A_KEY")
  cb=$(http_code GET /v1/models -H "Authorization: Bearer $TENANT_B_KEY")
  assert_eq EE-06 "租户A key 有效" 200 "$ca"
  assert_eq EE-06 "租户B key 有效" 200 "$cb"
  if require_upstream EE-07 "缓存租户隔离"; then
    p='企业缓存隔离测试问题ABC'
    http_body POST /v1/chat/completions -H "Authorization: Bearer $TENANT_A_KEY" "${JSON[@]}" -d "$(chat_body "$p")" >/dev/null
    sleep 1
    hdr=$(curl -s -D - -o /dev/null -X POST "$BASE/v1/chat/completions" -H "Authorization: Bearer $TENANT_B_KEY" "${JSON[@]}" -d "$(chat_body "$p")")
    if echo "$hdr" | grep -qi 'X-AegisGate-Tokens-Saved'; then
      _fail EE-07 "租户B 命中了租户A 的缓存（隔离失效）"
    else _pass EE-07 "租户B 未命中租户A 缓存（隔离正确）"; fi
  fi
else
  _skip EE-06 "未提供 TENANT_A_KEY/TENANT_B_KEY"
  _skip EE-07 "未提供两个租户 key"
fi

# ---------- 2.8 预算护栏 ----------
section "预算护栏 / 自治"
if require_upstream EE-35 "BudgetGuard 降级头"; then
  hdr=$(curl -s -D - -o /dev/null -X POST "$BASE/v1/chat/completions" "${AUTH[@]}" "${JSON[@]}" -d "$(chat_body '预算测试')")
  if echo "$hdr" | grep -qi 'X-AegisGate-Budget-Guard'; then _pass EE-35 "出现预算降级头"
  else _skip EE-35 "未出现降级头（需将 budget_guard.per_tenant_24h_usd 调到极低后复测）"; fi
fi

# ---------- 2.4 SCIM ----------
section "SCIM 2.0"
: "${SCIM_TOKEN:=}"
if [[ -n "$SCIM_TOKEN" ]]; then
  c=$(http_code GET /scim/v2/Users -H "Authorization: Bearer $SCIM_TOKEN")
  if [[ "$c" == "200" ]]; then _pass EE-23 "SCIM Users 列表"; else _fail EE-23 "SCIM Users -> $c"; fi
else
  _skip EE-23 "未提供 SCIM_TOKEN"
fi

# ---------- 构建受限 / 手工用例提示 ----------
section "需手工/构建支持的用例"
_skip EE-32 "插件系统：需 plugins.enabled + .so（见文档 2.7）"
_skip EE-33 "Agent 编排：需 agent.enabled（见文档 2.7）"
_skip EE-34 "RAG：需 rag.enabled（见文档 2.7）"
_skip EE-40 "集群分布式限流：需 -DENABLE_REDIS（见文档 2.9）"
_skip EE-41 "配置版本管理：需 -DENABLE_CONTROL_PLANE（见文档 2.9）"

summary
