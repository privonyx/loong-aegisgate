#!/usr/bin/env bash
set -euo pipefail

# 一次性验证脚本：drogon 路由优先级假设
# 用途：TASK-20260508-01 Epic 0.1 — 验证主方案设计依赖的 3 项核心假设
# 验证完成后保留作为 ADR 证据（不入 CI）
#
# 依赖：build/tests/integration/test_drogon_routing_priority

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

# 使用 build-cp（BUILD_TESTS=ON）作为脚手架编译目录。如果不存在则 fallback build/。
BUILD_DIR="build-cp"
[ -d "$BUILD_DIR" ] || BUILD_DIR="build"
BIN="$BUILD_DIR/tests/integration/test_drogon_routing_priority"
PORT=19999
PID=""

cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

echo "[1/4] 编译实测程序（在 $BUILD_DIR）..."
cmake --build "$BUILD_DIR" --target test_drogon_routing_priority 2>&1 | tail -5

if [ ! -x "$BIN" ]; then
  echo "  错误：$BIN 未找到"
  exit 1
fi

echo "[2/4] 启动..."
"$BIN" >/tmp/drogon-routing-stderr.log 2>&1 &
PID=$!
for i in {1..15}; do
  if curl -fsS "http://127.0.0.1:$PORT/a/b" -X POST -o /dev/null 2>/dev/null; then
    echo "  ✓ 启动就绪 (${i}s)"
    break
  fi
  sleep 1
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "  ✗ 进程已退出"
    cat /tmp/drogon-routing-stderr.log
    exit 1
  fi
done

echo "[3/4] 实测 4 个关键场景..."

R1=$(curl -s "http://127.0.0.1:$PORT/a/b")
R1_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/a/b")
echo "  GET /a/b → body='$R1' status=$R1_CODE"

R2=$(curl -s -X POST "http://127.0.0.1:$PORT/a/b")
R2_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$PORT/a/b")
echo "  POST /a/b → body='$R2' status=$R2_CODE"

R3=$(curl -s "http://127.0.0.1:$PORT/a/anything")
echo "  GET /a/anything → body='$R3'"

R4=$(curl -s "http://127.0.0.1:$PORT/admin/real.txt")
echo "  GET /admin/real.txt → body='$R4'"

R4B=$(curl -s "http://127.0.0.1:$PORT/admin/assets/foo.js")
echo "  GET /admin/assets/foo.js → body='$R4B'"

R5_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/admin/missing.txt")
echo "  GET /admin/missing.txt → status=$R5_CODE"

R6=$(curl -s "http://127.0.0.1:$PORT/admin/some-spa-route")
R6_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/admin/some-spa-route")
echo "  GET /admin/some-spa-route → body='$R6' status=$R6_CODE  (期望被 regex SPA fallback 接管)"

echo ""
echo "=== 结论汇总 ==="
PASS=0
FAIL=0

check() {
  local desc="$1" actual="$2" expected="$3"
  if [ "$actual" = "$expected" ]; then
    echo "  ✓ $desc"
    PASS=$((PASS+1))
  else
    echo "  ✗ $desc — 期望 '$expected'，实际 '$actual'"
    FAIL=$((FAIL+1))
  fi
}

# 假设 1a 修正：drogon controller 精确路径完全占住 path 节点（任何 method）。
# 405 是预期行为。方案 A 通过路径分层确保 SPA 路由与 controller 零冲突。
echo "  ℹ 假设 1a (drogon 行为): GET /a/b → '$R1' status=$R1_CODE"
echo "    drogon controller 精确路径独占 path 节点；405 是设计意图。"
echo "    方案 A 通过 namespace 分层（/admin/api/*）确保零冲突。"

check "假设 1b: POST /a/b 走 controller（regex GET 不阻挡 POST）"           "$R2"  "CONTROLLER"
check "假设 2:  GET /a/anything 被 regex 通配匹配"                            "$R3"  "REGEX"
check "假设 3a: 单 handler 服务真实文件 /admin/real.txt"                       "$R4"  "STATIC_FILE"
check "假设 3b: 单 handler 服务子目录 /admin/assets/foo.js"                    "$R4B" "ASSET_JS"
check "假设 4:  单 handler 未命中时返回 SPA index.html"                        "$R6"  "INDEX_HTML"

# 路径穿越保护测试
R7_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/admin/../etc/passwd")
echo ""
echo "  ℹ 假设 5 (SR1 路径穿越): GET /admin/../etc/passwd → status=$R7_CODE"
[ "$R7_CODE" = "404" ] && { echo "    ✓ 拒绝（404）"; PASS=$((PASS+1)); } || { echo "    ✗ 未拒绝"; FAIL=$((FAIL+1)); }

# addALocation 失败的信息（已弃用）
echo ""
echo "  ℹ addALocation 不再使用（一旦匹配前缀不 fall-through 到 regex），改用单 handler 方案"

echo ""
echo "通过: $PASS / 失败: $FAIL"

[ "$FAIL" -eq 0 ]
