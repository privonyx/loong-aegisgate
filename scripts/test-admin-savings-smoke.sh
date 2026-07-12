#!/usr/bin/env bash
set -euo pipefail

# 端到端 smoke 测试：Admin Savings Dashboard
# TASK-20260510-01 Epic 5.2
#
# 验证：
# 1. 未登录访问 /admin/api/savings/summary → 401
# 2. POST /admin/api/auth/login 拿到 cookie 后再访问 → 200 + 标准 schema 字段
# 3. 时间窗口 > 365 天 → 400 InvalidRequest（SR-NEW3）
# 4. 时间窗口合法（24h / 7d）→ 200
# 5. /admin/api/security/events 鉴权后正常返回
# 6. fallback_pricing_count / aggregator_since 字段存在（透明度 SR-NEW1）
# 7. SuperAdmin（默认 api_key 角色）的 top_tenants 字段存在（不一定有元素）
#
# 用法：
#   bash scripts/test-admin-savings-smoke.sh [aegisgate-binary] [admin-dist-dir]
#   默认: build/src/aegisgate web/admin/dist

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

BIN="${1:-build/src/aegisgate}"
DIST="${2:-web/admin/dist}"
PORT="${AEGISGATE_SAVINGS_SMOKE_PORT:-18081}"
TMPDIR_=$(mktemp -d)
chmod 700 "$TMPDIR_"
PID=""
PASS=0
FAIL=0

cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR_"
}
trap cleanup EXIT INT TERM

check_eq() {
  local desc="$1" actual="$2" expected="$3"
  if [ "$actual" = "$expected" ]; then
    echo "  ✓ $desc"
    PASS=$((PASS+1))
  else
    echo "  ✗ $desc — 期望 '$expected'，实际 '$actual'"
    FAIL=$((FAIL+1))
  fi
}

check_contains() {
  local desc="$1" actual="$2" needle="$3"
  if [[ "$actual" == *"$needle"* ]]; then
    echo "  ✓ $desc"
    PASS=$((PASS+1))
  else
    echo "  ✗ $desc — 未找到 '$needle' 于 [${actual:0:200}...]"
    FAIL=$((FAIL+1))
  fi
}

if [ ! -x "$BIN" ]; then
  echo "错误: aegisgate 二进制不存在或不可执行: $BIN"
  echo "提示: 先运行 cmake --build build --target aegisgate"
  exit 1
fi

if [ ! -f "$DIST/index.html" ]; then
  echo "错误: admin dist 中找不到 index.html: $DIST/index.html"
  echo "提示: 先运行 (cd web/admin && npm run build)"
  exit 1
fi

CONFIG="$TMPDIR_/aegisgate-savings-smoke.yaml"
cp "$ROOT_DIR/config/aegisgate.yaml" "$CONFIG"
sed -i \
  -e "s|^  port: 8080|  port: $PORT|" \
  -e "s|host: \"0.0.0.0\"|host: \"127.0.0.1\"|" \
  -e "s|persistent_backend: sqlite|persistent_backend: memory|" \
  -e "s|jwt_secret: \"\"|jwt_secret: \"savings-smoke-test-secret\"|" \
  -e "s|static_dir: \"web/admin/dist\"|static_dir: \"$ROOT_DIR/$DIST\"|" \
  "$CONFIG"

export AEGISGATE_API_KEY="savings-smoke-api-key"
export AEGISGATE_ADMIN_JWT_SECRET="savings-smoke-test-secret"

echo "[1/6] 启动 aegisgate (port=$PORT)..."
"$BIN" "$CONFIG" >"$TMPDIR_/aegisgate.log" 2>&1 &
PID=$!

for i in {1..30}; do
  if curl -fsS "http://127.0.0.1:$PORT/admin" -o /dev/null 2>/dev/null; then
    echo "  ✓ 启动就绪 (${i}s)"
    break
  fi
  sleep 1
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "  ✗ aegisgate 进程已退出"
    tail -20 "$TMPDIR_/aegisgate.log"
    exit 1
  fi
  if [ "$i" = "30" ]; then
    echo "  ✗ 30s 启动超时"
    tail -20 "$TMPDIR_/aegisgate.log"
    exit 1
  fi
done

echo ""
echo "[2/6] 未登录访问 → 401..."

S=$(curl -s -o "$TMPDIR_/anon" -w "%{http_code}" "http://127.0.0.1:$PORT/admin/api/savings/summary")
B=$(cat "$TMPDIR_/anon")
check_eq "GET /admin/api/savings/summary（无 cookie）→ 401" "$S" "401"
check_contains "未登录响应包含 error 字段" "$B" '"error"'

echo ""
echo "[3/6] POST /admin/api/auth/login..."

COOKIE_JAR="$TMPDIR_/cookie.txt"
S=$(curl -s -c "$COOKIE_JAR" -o "$TMPDIR_/login" -w "%{http_code}" \
  -X POST "http://127.0.0.1:$PORT/admin/api/auth/login" \
  -H "Content-Type: application/json" \
  -d "{\"api_key\":\"$AEGISGATE_API_KEY\"}")
LOGIN_BODY=$(cat "$TMPDIR_/login")
check_eq "POST /admin/api/auth/login → 200" "$S" "200"
check_contains "登录响应含 user_id" "$LOGIN_BODY" '"user_id"'
check_contains "登录响应含 role" "$LOGIN_BODY" '"role"'

if ! grep -q aegis_session "$COOKIE_JAR"; then
  echo "  ✗ 未拿到 aegis_session cookie，后续测试无法继续"
  cat "$COOKIE_JAR"
  exit 1
fi

echo ""
echo "[4/6] /admin/api/savings/summary 标准请求..."

S=$(curl -s -b "$COOKIE_JAR" -o "$TMPDIR_/savings7d" -w "%{http_code}" \
  "http://127.0.0.1:$PORT/admin/api/savings/summary")
B=$(cat "$TMPDIR_/savings7d")
check_eq "GET /admin/api/savings/summary（默认窗口）→ 200" "$S" "200"
check_contains "返回含 total_cost_saved" "$B" '"total_cost_saved"'
check_contains "返回含 total_tokens_saved" "$B" '"total_tokens_saved"'
check_contains "返回含 by_type" "$B" '"by_type"'
check_contains "返回含 by_model" "$B" '"by_model"'
check_contains "返回含 time_series" "$B" '"time_series"'
check_contains "返回含 routing_recommendations" "$B" '"routing_recommendations"'
check_contains "返回含 top_tenants（SuperAdmin 视角）" "$B" '"top_tenants"'

# SR-NEW1 透明度
check_contains "返回含 fallback_pricing_count（SR-NEW1）" "$B" '"fallback_pricing_count"'
check_contains "返回含 aggregator_since" "$B" '"aggregator_since"'
check_contains "返回含 roi_percent" "$B" '"roi_percent"'

# 显式 24h / 7d
NOW_ISO=$(date -u -Iseconds | sed 's/+00:00/Z/')
SEVENDAYS_ISO=$(date -u -d '7 days ago' -Iseconds 2>/dev/null | sed 's/+00:00/Z/' \
                || date -u -v-7d -Iseconds | sed 's/+00:00/Z/')

S=$(curl -s -b "$COOKIE_JAR" -o /dev/null -w "%{http_code}" \
  "http://127.0.0.1:$PORT/admin/api/savings/summary?from=${SEVENDAYS_ISO}&to=${NOW_ISO}")
check_eq "GET /admin/api/savings/summary?from=7d&to=now → 200" "$S" "200"

echo ""
echo "[5/6] SR-NEW3 时间窗口 > 365 天拒绝..."

# 选 400 天窗口
FAR_PAST_ISO=$(date -u -d '400 days ago' -Iseconds 2>/dev/null | sed 's/+00:00/Z/' \
                || date -u -v-400d -Iseconds | sed 's/+00:00/Z/')
S=$(curl -s -b "$COOKIE_JAR" -o "$TMPDIR_/big" -w "%{http_code}" \
  "http://127.0.0.1:$PORT/admin/api/savings/summary?from=${FAR_PAST_ISO}&to=${NOW_ISO}")
B=$(cat "$TMPDIR_/big")
check_eq "GET /admin/api/savings/summary?from=400d → 400 (SR-NEW3 时间窗口拒绝)" "$S" "400"
check_contains "400 响应含 error 字段" "$B" '"error"'

echo ""
echo "[6/6] /admin/api/security/events 鉴权后正常返回..."

S=$(curl -s -b "$COOKIE_JAR" -o "$TMPDIR_/sec" -w "%{http_code}" \
  "http://127.0.0.1:$PORT/admin/api/security/events")
B=$(cat "$TMPDIR_/sec")
check_eq "GET /admin/api/security/events → 200" "$S" "200"

# 同时确认未登录访问 security/events 也被拒
S=$(curl -s -o /dev/null -w "%{http_code}" \
  "http://127.0.0.1:$PORT/admin/api/security/events")
check_eq "GET /admin/api/security/events（无 cookie）→ 401" "$S" "401"

echo ""
echo "================================="
echo "通过: $PASS / 失败: $FAIL"
echo "================================="

if [ "$FAIL" -ne 0 ]; then
  echo ""
  echo "失败时的 aegisgate 日志（最后 30 行）："
  tail -30 "$TMPDIR_/aegisgate.log"
fi

[ "$FAIL" -eq 0 ]
