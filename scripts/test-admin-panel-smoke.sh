#!/usr/bin/env bash
set -euo pipefail

# 端到端 smoke 测试：admin 面板路由完整链路
# TASK-20260508-01 Epic 3.3
#
# 验证：
# 1. SPA 入口（GET /admin、/admin/、/admin/login、/admin/任意路径 → 200 + index.html）
# 2. 静态资源（GET /admin/assets/* → 200 + 正确 MIME）
# 3. API 路径（GET /admin/api/* → 由 controller 处理）
# 4. 老路径不再可用（POST /admin/login → 405 / 404，证明已迁到 /admin/api/auth/login）
# 5. 根路径无 SPA 资源泄漏（GET / → 不返回 SPA HTML，GET /assets/* → 404）
# 6. 安全：路径穿越拒绝（GET /admin/../etc/passwd → 拒绝）
#
# 用法：
#   bash scripts/test-admin-panel-smoke.sh [aegisgate-binary] [admin-dist-dir]
#   默认: build/aegisgate web/admin/dist

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

BIN="${1:-build/src/aegisgate}"
DIST="${2:-web/admin/dist}"
PORT="${AEGISGATE_SMOKE_PORT:-18080}"
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
    echo "  ✗ $desc — 未找到 '$needle' 于 [$actual]"
    FAIL=$((FAIL+1))
  fi
}

check_not_contains() {
  local desc="$1" actual="$2" needle="$3"
  if [[ "$actual" == *"$needle"* ]]; then
    echo "  ✗ $desc — 不应包含 '$needle' 但包含了"
    FAIL=$((FAIL+1))
  else
    echo "  ✓ $desc"
    PASS=$((PASS+1))
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

ASSET_FILE=$(ls "$DIST/assets/"*.js 2>/dev/null | head -1)
if [ -z "$ASSET_FILE" ]; then
  echo "错误: 未找到 dist/assets/*.js 用于资源服务测试"
  exit 1
fi
ASSET_NAME=$(basename "$ASSET_FILE")

# 复用 repo 默认 config，仅 sed 覆盖关键字段（避免 yaml-cpp 严格 key 校验）
CONFIG="$TMPDIR_/aegisgate-smoke.yaml"
cp "$ROOT_DIR/config/aegisgate.yaml" "$CONFIG"
sed -i \
  -e "s|^  port: 8080|  port: $PORT|" \
  -e "s|host: \"0.0.0.0\"|host: \"127.0.0.1\"|" \
  -e "s|persistent_backend: sqlite|persistent_backend: memory|" \
  -e "s|jwt_secret: \"\"|jwt_secret: \"smoke-test-secret\"|" \
  -e "s|static_dir: \"web/admin/dist\"|static_dir: \"$ROOT_DIR/$DIST\"|" \
  "$CONFIG"

# 必要环境变量（auth.api_keys 模板替换）
export AEGISGATE_API_KEY="smoke-test-api-key"
export AEGISGATE_ADMIN_JWT_SECRET="smoke-test-secret"

echo "[1/5] 启动 aegisgate (port=$PORT, dist=$DIST)..."
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
echo "[2/5] SPA 入口测试..."

S=$(curl -s -o "$TMPDIR_/r1" -w "%{http_code}" "http://127.0.0.1:$PORT/admin")
B=$(cat "$TMPDIR_/r1")
check_eq "GET /admin → 200" "$S" "200"
check_contains "GET /admin → 包含 SPA <div id=\"root\">" "$B" 'id="root"'

S=$(curl -s -o "$TMPDIR_/r2" -w "%{http_code}" "http://127.0.0.1:$PORT/admin/")
check_eq "GET /admin/ → 200" "$S" "200"

S=$(curl -s -o "$TMPDIR_/r3" -w "%{http_code}" "http://127.0.0.1:$PORT/admin/login")
B=$(cat "$TMPDIR_/r3")
check_eq "GET /admin/login → 200 (SPA fallback)" "$S" "200"
check_contains "GET /admin/login → 是 SPA HTML 不是 404" "$B" 'id="root"'

S=$(curl -s -o "$TMPDIR_/r4" -w "%{http_code}" "http://127.0.0.1:$PORT/admin/tenants/some-id-deep")
check_eq "GET /admin/tenants/some-id-deep → 200 (SPA fallback)" "$S" "200"

echo ""
echo "[3/5] 静态资源服务测试..."

S=$(curl -s -o "$TMPDIR_/asset" -w "%{http_code}" "http://127.0.0.1:$PORT/admin/assets/$ASSET_NAME")
CT=$(curl -sI "http://127.0.0.1:$PORT/admin/assets/$ASSET_NAME" | tr -d '\r' | grep -i '^content-type:' | awk '{print $2}')
check_eq "GET /admin/assets/$ASSET_NAME → 200" "$S" "200"
check_contains "/admin/assets/*.js → Content-Type 含 javascript" "$CT" "javascript"

# 真实文件大小一致
EXPECTED_SIZE=$(stat -c%s "$ASSET_FILE")
ACTUAL_SIZE=$(stat -c%s "$TMPDIR_/asset")
check_eq "/admin/assets/$ASSET_NAME → 内容大小匹配 dist" "$ACTUAL_SIZE" "$EXPECTED_SIZE"

echo ""
echo "[4/5] API 路径与老路径迁移测试..."

# 旧路径 POST /admin/login 应已不可用（404 或 405 都可——controller 已迁走）
S=$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$PORT/admin/login" \
       -H "Content-Type: application/json" -d '{"api_key":"x"}')
if [ "$S" = "404" ] || [ "$S" = "200" ]; then
  # 200 = SPA fallback 接管（GET handler 不响应 POST 但 SPA fallback 是 GET only,
  #       所以 POST 实际不会得到 200。如果是 200 说明 fallback 错误覆盖了 POST）
  if [ "$S" = "200" ]; then
    echo "  ✗ POST /admin/login 不应返回 200（不应被 SPA HTML 覆盖 POST 请求）"
    FAIL=$((FAIL+1))
  else
    echo "  ✓ POST /admin/login → 404 (老路径已迁走)"
    PASS=$((PASS+1))
  fi
else
  echo "  ✓ POST /admin/login → $S (非 200，老路径已迁走)"
  PASS=$((PASS+1))
fi

# 新路径 POST /admin/api/auth/login 应可用（无效 key 应返回 4xx 而非 404）
S=$(curl -s -o "$TMPDIR_/login" -w "%{http_code}" -X POST "http://127.0.0.1:$PORT/admin/api/auth/login" \
       -H "Content-Type: application/json" -d '{"api_key":"invalid-test-key"}')
B=$(cat "$TMPDIR_/login")
if [ "$S" != "404" ] && [ "$S" != "405" ]; then
  echo "  ✓ POST /admin/api/auth/login → $S (新路径已注册，非 404/405)"
  PASS=$((PASS+1))
else
  echo "  ✗ POST /admin/api/auth/login → $S (新路径未注册或方法不允许)"
  echo "    response body: $B"
  FAIL=$((FAIL+1))
fi

echo ""
echo "[5/6] 根路径无泄漏 + 安全测试..."

# 根路径不应返回 SPA HTML
S=$(curl -s -o "$TMPDIR_/root" -w "%{http_code}" "http://127.0.0.1:$PORT/")
B=$(cat "$TMPDIR_/root")
check_not_contains "GET / → 不应返回 SPA HTML（root 不被污染）" "$B" 'id="root"'

# 根路径下访问 assets 不应命中（之前 setDocumentRoot 会泄漏到这里）
S=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/assets/$ASSET_NAME")
if [ "$S" = "404" ]; then
  echo "  ✓ GET /assets/$ASSET_NAME → 404 (根路径无 SPA 资源泄漏)"
  PASS=$((PASS+1))
else
  echo "  ✗ GET /assets/$ASSET_NAME → $S (根路径泄漏 SPA 资源 — setDocumentRoot 残留？)"
  FAIL=$((FAIL+1))
fi

# SR1 路径穿越
S=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/admin/../etc/passwd")
if [ "$S" = "404" ] || [ "$S" = "400" ]; then
  echo "  ✓ GET /admin/../etc/passwd → $S (路径穿越拒绝)"
  PASS=$((PASS+1))
else
  echo "  ✗ GET /admin/../etc/passwd → $S (路径穿越未拒绝)"
  FAIL=$((FAIL+1))
fi

# /admin/missing-spa-route → 应返回 SPA index.html (200)
S=$(curl -s -o "$TMPDIR_/spa" -w "%{http_code}" "http://127.0.0.1:$PORT/admin/no-such-page")
B=$(cat "$TMPDIR_/spa")
check_eq "GET /admin/no-such-page → 200 (SPA fallback)" "$S" "200"
check_contains "GET /admin/no-such-page → 是 SPA HTML" "$B" 'id="root"'

# 防御性排除：/admin/api 但路径不存在 → controller 优先级应让其不落到 SPA
S=$(curl -s -o "$TMPDIR_/apifb" -w "%{http_code}" "http://127.0.0.1:$PORT/admin/api/nonexistent")
B=$(cat "$TMPDIR_/apifb")
check_not_contains "GET /admin/api/nonexistent → 不应是 SPA HTML（API namespace 隔离）" "$B" 'id="root"'

echo ""
echo "[6/6] Guard 端点 cookie 作用域回归锁 (TASK-20260706-01 / SR-1 + SR-2)..."

# SR-1：未认证访问 guard 端点 → 401（路由迁移不削弱鉴权）。
S=$(curl -s -o /dev/null -w "%{http_code}" \
       "http://127.0.0.1:$PORT/admin/api/guard/explanation/no-such-request-id")
check_eq "GET /admin/api/guard/explanation/<none> 未认证 → 401 (SR-1 鉴权不削弱)" "$S" "401"

# 登录换取 admin session cookie（cookie 为 Path=/admin；仅存入 jar 文件，不打印明文 token）。
GUARD_CK="$TMPDIR_/guard_cookie.txt"
curl -s -o /dev/null -c "$GUARD_CK" -X POST "http://127.0.0.1:$PORT/admin/api/auth/login" \
       -H "Content-Type: application/json" \
       -d '{"api_key":"'"$AEGISGATE_API_KEY"'"}'

# SR-2：带 admin cookie 访问 guard 端点 → 非 401（认证通过 = cookie 随
# /admin/api/guard 送达）。此为 cookie 作用域回归锁的精确语义：
#   - 修复前 (/v1/guard)：cookie(Path=/admin) 不匹配 /v1 → 恒 401（作用域断裂）
#   - 修复后 (/admin/api/guard)：认证通过 → 业务码（本地默认 config 未装配
#     Adaptive Guard → 503；装配后无该记录 → 404）——均非 401。
# 若有人把路由改回 /v1/guard，此断言立即变 401 FAIL，锁死回归。
S=$(curl -s -o "$TMPDIR_/guard_expl" -w "%{http_code}" -b "$GUARD_CK" \
       "http://127.0.0.1:$PORT/admin/api/guard/explanation/no-such-request-id")
if [ "$S" != "401" ]; then
  echo "  ✓ GET /admin/api/guard/explanation/<none> 已认证 → $S 非 401 (SR-2 cookie 作用域对齐)"
  PASS=$((PASS+1))
else
  echo "  ✗ GET /admin/api/guard/explanation/<none> 已认证 → 401 (cookie 未随 /admin/api/guard 送达 — 作用域断裂回归!)"
  FAIL=$((FAIL+1))
fi

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
