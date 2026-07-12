#!/usr/bin/env bash
# TASK-20260617-02 — 仪表盘数据持久化 端到端 smoke。
#
# 验证「重启后非零」核心契约，且无需上游 / 无需 admin 登录：
#   1. 首次启动 server（sqlite 后端，临时 DB）→ 自动建表 savings_events / cost_records。
#   2. 直接向 DB 注入近 N 天的 cost_records + savings_events 行（模拟重启前已有数据）。
#   3. 二次启动 server（同一 DB）→ 装配期触发 CostTracker.loadFromStore +
#      SavingsAggregator.loadFromStore，断言启动日志显示「replayed >0」。
#
# 这覆盖了真实修复路径（DB → loadFromStore → 内存聚合恢复），是重启持久化的确定性证据。
#
# 用法：
#   ./scripts/test-dashboard-persistence-smoke.sh
#   BIN=build/src/aegisgate CONFIG=config/aegisgate.yaml ./scripts/test-dashboard-persistence-smoke.sh
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

BIN="${BIN:-build/src/aegisgate}"
CONFIG="${CONFIG:-config/aegisgate.yaml}"
PORT="${PORT:-18099}"

PASS=0
FAIL=0
pass() { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
skip() { echo "  [SKIP] $*"; }

# --- 前置检查 ---
if [[ ! -x "$BIN" ]]; then
  skip "未找到可执行 $BIN（先构建：cmake --build build --target aegisgate）"
  exit 0
fi
PY="$(command -v python3 || command -v python || true)"
if [[ -z "$PY" ]]; then
  skip "未找到 python3（注入/检查步骤依赖其 sqlite3 模块）"
  exit 0
fi
if ! "$PY" -c 'import sqlite3' >/dev/null 2>&1; then
  skip "python3 缺少 sqlite3 模块"
  exit 0
fi

# DB 辅助：用 python3 stdlib sqlite3（环境无 sqlite3 CLI）。
db_tables() { "$PY" - "$DB" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
print(" ".join(r[0] for r in c.execute(
    "SELECT name FROM sqlite_master WHERE type='table'")))
PY
}
db_count() { "$PY" - "$DB" "$1" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
print(c.execute("SELECT COUNT(*) FROM %s" % sys.argv[2]).fetchone()[0])
PY
}
db_seed() { "$PY" - "$DB" "$1" <<'PY'
import sqlite3, sys
db, now = sys.argv[1], sys.argv[2]
c = sqlite3.connect(db)
c.execute(
    "INSERT INTO cost_records (request_id, tenant_id, app_id, model, "
    "input_tokens, output_tokens, input_cost, output_cost, total_cost, "
    "timestamp, modality, baseline_cost, routing_decision_reason) "
    "VALUES ('smoke-r1','t1','app1','gpt-4',100,50,0.3,0.3,0.6,?,'chat',1.2,'')",
    (now,))
c.executemany(
    "INSERT INTO savings_events (type, model, tenant_id, tokens_saved, "
    "cost_saved, fallback_pricing, timestamp) VALUES (?,?,?,?,?,?,?)",
    [(0, 'gpt-4', 't1', 150, 0.45, 0, now),
     (1, 'gpt-4', 't2', 80, 0.20, 0, now)])
c.commit()
PY
}

TMPDIR="$(mktemp -d)"
DB="$TMPDIR/dashboard-smoke.db"
LOG1="$TMPDIR/run1.log"
LOG2="$TMPDIR/run2.log"
cleanup() {
  [[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

export AEGISGATE_SQLITE_PATH="$DB"
export AEGISGATE_PORT="$PORT"

start_server() {
  local log="$1"
  "$BIN" "$CONFIG" >"$log" 2>&1 &
  PID=$!
  # 等待启动（建表 + loadFromStore 在监听前完成）；最多 ~15s。
  for _ in $(seq 1 150); do
    if grep -qE "Persistent store:|loadFromStore|Listening|server" "$log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
      return 1  # 进程已退出
    fi
    sleep 0.1
  done
  return 0
}

echo "== Dashboard persistence smoke =="
echo "DB=$DB  PORT=$PORT"

# --- 1. 首启：建表 ---
echo "[1] 首次启动（建表）"
start_server "$LOG1"
sleep 1
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""

if db_tables | grep -q savings_events; then
  pass "savings_events 表已创建"
else
  fail "savings_events 表未创建（日志见 $LOG1）"
  echo "---- run1.log tail ----"; tail -15 "$LOG1"
  echo "PASS=$PASS FAIL=$FAIL"; exit 1
fi

# --- 2. 注入近期数据（模拟重启前已有仪表盘数据）---
echo "[2] 注入 cost_records + savings_events"
NOW_ISO="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
db_seed "$NOW_ISO"
echo "  注入后：cost_records=$(db_count cost_records) savings_events=$(db_count savings_events)"

# --- 3. 二启：断言回放非零 ---
echo "[3] 二次启动（应回放历史数据）"
start_server "$LOG2"
sleep 1
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""

if grep -qE "CostTracker loadFromStore: replayed [1-9][0-9]* cost records" "$LOG2"; then
  pass "CostTracker 回放非零成本记录"
else
  fail "CostTracker 未回放非零记录"
  grep -i "loadFromStore" "$LOG2" || echo "  (日志无 loadFromStore 行，tail：)" && tail -15 "$LOG2"
fi

if grep -qE "SavingsAggregator loadFromStore: replayed [1-9][0-9]* savings events" "$LOG2"; then
  pass "SavingsAggregator 回放非零节省事件"
else
  fail "SavingsAggregator 未回放非零事件"
  grep -i "loadFromStore" "$LOG2" || tail -15 "$LOG2"
fi

echo
echo "== 结果：PASS=$PASS FAIL=$FAIL =="
[[ "$FAIL" -eq 0 ]] && exit 0 || exit 1
