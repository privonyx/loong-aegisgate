#!/usr/bin/env bash
# TASK-20260622-01 E2 — scripts/smoke-prod.sh 纯函数离线单测。
#
# 沿用 shell 函数抽取 + fixture 范式（参考 test_download_model_integrity.sh）：source 脚本
# （main 由 BASH_SOURCE 守卫不执行），逐函数喂 fixture 断言：
#   - redact          脱敏 postgres 口令 / api-key / jwt / Bearer（SR1）
#   - assert_capability_summary  cmake 五行 [ON ]（缺一即失败）
#   - assert_startup_log         四后端生效启动日志（缺一即失败）
#   - assert_health_ready        /health/ready JSON backend/healthy/status
#
# 用法: bash tests/scripts/test_smoke_prod.sh   退出 0=全过 1=任一失败

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SMOKE="$ROOT_DIR/scripts/smoke-prod.sh"

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

if [[ ! -f "$SMOKE" ]]; then
    echo "  [FAIL] scripts/smoke-prod.sh 不存在"
    exit 1
fi
# shellcheck source=/dev/null
source "$SMOKE"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ---------- redact（SR1）----------
echo "==> redact"
secret_in='POSTGRES_URL=postgres://aegis:s3cretPass@db:5432/ag api_key=sk-abc123XYZ Authorization: Bearer eyJhbGc.eyJzdWI.sigPart token="eyJ0eXAi.eyJpZCI6.zzz"'
red="$(printf '%s\n' "$secret_in" | redact)"
if ! printf '%s' "$red" | grep -Eq 's3cretPass|sk-abc123XYZ|eyJhbGc\.eyJzdWI\.sigPart'; then
    pass "redact 移除了口令/api-key/jwt 明文"
else
    fail "redact 仍泄漏明文: $red"
fi
if printf '%s' "$red" | grep -Fq "REDACTED"; then
    pass "redact 保留 [REDACTED] 占位标记"
else
    fail "redact 未留占位标记: $red"
fi
# SR1 硬断言：脱敏输出不得含任何敏感 token
if printf '%s' "$red" | grep -Eq 'secret|s3cret|sk-[A-Za-z0-9]{6,}|Bearer [A-Za-z0-9]'; then
    fail "SR1: redact 输出仍匹配敏感模式: $red"
else
    pass "SR1: redact 输出无敏感模式残留"
fi

# ---------- assert_capability_summary ----------
echo "==> assert_capability_summary"
cat > "$TMP/cmake_green.log" <<'EOF'
-- ================ AegisGate 构建能力摘要 ================
--   [ON ] ONNX 安全护栏模型 (ENABLE_GUARD_MODEL)
--   [ON ] Redis 共享缓存 (ENABLE_REDIS)
--   [ON ] PostgreSQL 持久化 (ENABLE_PG)
--   [ON ] OpenTelemetry 分布式追踪 (ENABLE_OPENTELEMETRY)
--   [ON ] gRPC 控制面 (ENABLE_CONTROL_PLANE)
-- =======================================================
EOF
if assert_capability_summary "$TMP/cmake_green.log" >/dev/null 2>&1; then
    pass "全绿能力摘要 → PASS"
else
    fail "全绿能力摘要被误判为 FAIL"
fi
# 缺 Redis 一行
grep -v "ENABLE_REDIS" "$TMP/cmake_green.log" > "$TMP/cmake_missing.log"
if assert_capability_summary "$TMP/cmake_missing.log" >/dev/null 2>&1; then
    fail "缺 ENABLE_REDIS 仍被判 PASS"
else
    pass "缺 ENABLE_REDIS → FAIL"
fi
# Redis 为 [off]（非生产档位）
sed 's/\[ON \] Redis/[off] Redis/' "$TMP/cmake_green.log" > "$TMP/cmake_off.log"
if assert_capability_summary "$TMP/cmake_off.log" >/dev/null 2>&1; then
    fail "Redis [off] 仍被判 PASS"
else
    pass "Redis [off] → FAIL"
fi

# ---------- assert_startup_log ----------
echo "==> assert_startup_log"
cat > "$TMP/app_green.log" <<'EOF'
[info] AegisGate v1.0.0 starting...
[info] Cache store: redis
[info] Persistent store: postgres
[info] OpenTelemetry tracing initialized: endpoint=http://localhost:4318, service=aegisgate
[info] GuardClassifier: local ONNX guard model active
[info] Listening on 0.0.0.0:8080
EOF
if assert_startup_log "$TMP/app_green.log" >/dev/null 2>&1; then
    pass "四后端生效启动日志 → PASS"
else
    fail "四后端生效启动日志被误判 FAIL"
fi
# 回退 memory（误配 / 假阳性）
sed 's/Cache store: redis/Cache store: memory/' "$TMP/app_green.log" > "$TMP/app_mem.log"
if assert_startup_log "$TMP/app_mem.log" >/dev/null 2>&1; then
    fail "Cache store: memory 仍被判 PASS（假阳性未拦截）"
else
    pass "Cache store: memory → FAIL"
fi
# guard_mode=any（CI）：guard pass-through 编译证明应 PASS
sed 's/GuardClassifier: local ONNX guard model active/GuardClassifier: L3 disabled (no model) — running fail-open pass-through/' \
    "$TMP/app_green.log" > "$TMP/app_passthrough.log"
if assert_startup_log "$TMP/app_passthrough.log" any >/dev/null 2>&1; then
    pass "guard pass-through + mode=any → PASS（CI 编译证明）"
else
    fail "guard pass-through + mode=any 被误判 FAIL"
fi
# guard_mode=active（real）：pass-through 不够，应 FAIL
if assert_startup_log "$TMP/app_passthrough.log" active >/dev/null 2>&1; then
    fail "guard pass-through + mode=active 仍被判 PASS（real 应严格）"
else
    pass "guard pass-through + mode=active → FAIL（real 严格）"
fi
# 完全无 guard 行 + mode=any：应 FAIL（未编入）
grep -v "GuardClassifier" "$TMP/app_green.log" > "$TMP/app_noguard.log"
if assert_startup_log "$TMP/app_noguard.log" any >/dev/null 2>&1; then
    fail "无 guard 行 + mode=any 仍被判 PASS（未编入未拦截）"
else
    pass "无 guard 行 + mode=any → FAIL"
fi

# ---------- assert_health_ready ----------
echo "==> assert_health_ready"
cat > "$TMP/ready_green.json" <<'EOF'
{"status":"ready","version":"1.0.0","checks":{"runtime":true,"shutting_down":false,"persistent_store":{"healthy":true,"backend":"postgres"},"cache_store":{"healthy":true,"backend":"redis"}}}
EOF
if assert_health_ready "$TMP/ready_green.json" >/dev/null 2>&1; then
    pass "ready+redis+postgres → PASS"
else
    fail "ready+redis+postgres 被误判 FAIL"
fi
cat > "$TMP/ready_mem.json" <<'EOF'
{"status":"ready","version":"1.0.0","checks":{"runtime":true,"shutting_down":false,"persistent_store":{"healthy":true,"backend":"memory"},"cache_store":{"healthy":true,"backend":"memory"}}}
EOF
if assert_health_ready "$TMP/ready_mem.json" >/dev/null 2>&1; then
    fail "backend=memory 仍被判 PASS（假阳性未拦截）"
else
    pass "backend=memory → FAIL"
fi
cat > "$TMP/ready_degraded.json" <<'EOF'
{"status":"degraded","version":"1.0.0","checks":{"runtime":true,"shutting_down":false,"persistent_store":{"healthy":false,"backend":"postgres"},"cache_store":{"healthy":true,"backend":"redis"}}}
EOF
if assert_health_ready "$TMP/ready_degraded.json" >/dev/null 2>&1; then
    fail "status=degraded 仍被判 PASS"
else
    pass "status=degraded → FAIL"
fi

# ---------- upstream_payload（真实上游模型需与 models.yaml 对齐）----------
echo "==> upstream_payload"
payload="$(upstream_payload deepseek-chat)"
if printf '%s' "$payload" | grep -Fq '"model":"deepseek-chat"' && \
   ! printf '%s' "$payload" | grep -Fq 'gpt-3.5-turbo'; then
    pass "上游 smoke payload 使用传入模型 deepseek-chat"
else
    fail "上游 smoke payload 未使用 deepseek-chat: $payload"
fi

# ---------- detect_startup_problem（自起实例死亡定位 / 防误连陌生实例）----------
echo "==> detect_startup_problem"
# strict-abort 日志 → 应给出"strict 拒绝启动/缺环境变量"原因
cat > "$TMP/app_abort.log" <<'EOF'
[info] Cache store: redis
[error] PG connect error: connection refused
[info] Persistent store: memory
[critical] Requested persistent backend 'postgres' is NOT active [storage.strict_backends=true -> refusing to start]
[critical] Startup aborted: ...
EOF
why="$(detect_startup_problem "$TMP/app_abort.log")"
if [[ -n "$why" ]] && echo "$why" | grep -q "strict"; then
    pass "strict-abort 日志 → 定位到 strict 拒绝启动"
else
    fail "strict-abort 日志应定位 strict 原因，得到: [$why]"
fi
# 端口占用日志 → 应给出端口占用原因
cat > "$TMP/app_bind.log" <<'EOF'
[info] Cache store: redis
[error] Failed to listen: address already in use
EOF
why="$(detect_startup_problem "$TMP/app_bind.log")"
if [[ -n "$why" ]] && echo "$why" | grep -q "端口"; then
    pass "bind 失败日志 → 定位到端口占用"
else
    fail "bind 失败应定位端口占用，得到: [$why]"
fi
# 健康完整启动日志（有 Listening on）→ 应无问题（空）
why="$(detect_startup_problem "$TMP/app_green.log")"
if [[ -z "$why" ]]; then
    pass "完整启动日志(含 Listening on) → 无问题"
else
    fail "完整启动日志不应报问题，得到: [$why]"
fi
# 日志缺失 → 应报"进程未拉起"
why="$(detect_startup_problem "$TMP/nonexistent.log")"
if [[ -n "$why" ]]; then
    pass "日志缺失 → 报进程未拉起"
else
    fail "日志缺失应报问题"
fi

echo ""
echo "================================="
echo "通过: $PASS / 失败: $FAIL"
echo "================================="
[[ "$FAIL" -eq 0 ]]
