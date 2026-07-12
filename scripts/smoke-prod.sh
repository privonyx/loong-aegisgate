#!/usr/bin/env bash
# TASK-20260622-01 E2 — 生产档位端到端冒烟「证据包」脚本。
#
# 目标：把「生产档位（Redis+PG+OTel+Guard 全开）能真实构建、启动不段错误、
# 请求贯通、各后端真实生效」固化为可重复、确定性的断言，任何人/CI 一键复跑。
#
# 用法:
#   smoke-prod.sh [--mode real|ci] [--with-upstream] [--host H] [--port P]
#                 [--api-key KEY] [--upstream-model MODEL] [--config FILE] [--binary FILE]
#                 [--cmake-log FILE] [--out DIR] [--no-start]
#
#   --mode real （默认）  完整证据包（含 redis-cli/psql/journalctl 侧证）。
#   --mode ci            去掉需 root/systemd 的侧证；默认不打真实上游。
#   --with-upstream      发真实 /v1/chat/completions（需上游 key）。
#   --upstream-model     上游 smoke 模型（默认 deepseek-chat，可用 SMOKE_UPSTREAM_MODEL 覆盖）。
#   --cmake-log FILE     构建期 cmake 能力摘要日志（断言五行 [ON ]）。
#   --out DIR            证据输出目录（默认 .smoke-out / 已 .gitignore）。
#   --no-start           不由脚本拉起进程（断言已在运行的实例）。
#
# 退出码: 0=全部 PASS；非 0=任一断言 FAIL。
# 安全(SR1/SR3): 所有 stdout 与 --out 证据文件经 redact 脱敏；证据文件 chmod 600；
#               默认 out 目录在 .gitignore 内，不进版本控制。

set -uo pipefail

# ============================================================
# 纯函数区（可被 tests/scripts/test_smoke_prod.sh source 后单测）
# ============================================================

# redact: stdin → stdout，移除口令 / api-key / jwt / Bearer / KEY=VALUE 敏感值（SR1）
redact() {
    sed -E \
        -e 's#(postgres(ql)?://[^:@/[:space:]]+:)[^@[:space:]]+@#\1[REDACTED]@#g' \
        -e 's#(redis://[^:@/[:space:]]+:)[^@[:space:]]+@#\1[REDACTED]@#g' \
        -e 's#[Bb]earer [A-Za-z0-9._~+/=-]+#Bearer [REDACTED]#g' \
        -e 's#eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+#[REDACTED_JWT]#g' \
        -e 's#sk-[A-Za-z0-9_-]{6,}#sk-[REDACTED]#g' \
        -e 's#(([Aa][Pp][Ii]_?[Kk][Ee][Yy]|[Pp][Aa][Ss][Ss][Ww][Oo][Rr][Dd]|[Ss][Ee][Cc][Rr][Ee][Tt]|[Tt][Oo][Kk][Ee][Nn])["'"'"']?[[:space:]]*[=:][[:space:]]*["'"'"']?)[^"'"'"'[:space:]]+#\1[REDACTED]#g'
}

# assert_capability_summary <cmake_log>: 五条生产能力开关均为 [ON ]
assert_capability_summary() {
    local f="$1" missing=0 flag
    [[ -f "$f" ]] || { echo "  [FAIL] cmake 日志不存在: $f"; return 1; }
    for flag in ENABLE_REDIS ENABLE_PG ENABLE_OPENTELEMETRY ENABLE_CONTROL_PLANE ENABLE_GUARD_MODEL; do
        if grep -Eq "\[ON \].*\(${flag}\)" "$f"; then
            echo "  [PASS] 能力 ${flag} = ON"
        else
            echo "  [FAIL] 能力 ${flag} 非 ON（非生产档位）"
            missing=$((missing + 1))
        fi
    done
    [[ "$missing" -eq 0 ]]
}

# assert_startup_log <app_log> [guard_mode]: 后端「真实生效」启动日志齐全。
# guard_mode=active（默认 / real）：强求 guard 本地模型 active。
# guard_mode=any   （ci）：接受 active 或 pass-through 编译证明（避免 PR 下大模型）。
assert_startup_log() {
    local f="$1" guard_mode="${2:-active}" missing=0 pair pat desc
    [[ -f "$f" ]] || { echo "  [FAIL] 启动日志不存在: $f"; return 1; }
    local -a checks=(
        "Cache store: redis|缓存后端 redis 生效"
        "Persistent store: postgres|持久化后端 postgres 生效"
        "OpenTelemetry tracing initialized|OTel 追踪已初始化"
    )
    for pair in "${checks[@]}"; do
        pat="${pair%%|*}"; desc="${pair##*|}"
        if grep -Fq "$pat" "$f"; then
            echo "  [PASS] $desc"
        else
            echo "  [FAIL] 缺启动证据: $desc"
            missing=$((missing + 1))
        fi
    done
    # Guard：active（严）或 active|pass-through（编译证明，CI 宽）
    if [[ "$guard_mode" == "any" ]]; then
        if grep -Eq "GuardClassifier: (local ONNX guard model active|L3 disabled)" "$f"; then
            echo "  [PASS] Guard 编入并运行（active 或 pass-through）"
        else
            echo "  [FAIL] 缺启动证据: Guard 未编入/未运行"
            missing=$((missing + 1))
        fi
    else
        if grep -Fq "GuardClassifier: local ONNX guard model active" "$f"; then
            echo "  [PASS] Guard 本地模型生效"
        else
            echo "  [FAIL] 缺启动证据: Guard 本地模型生效"
            missing=$((missing + 1))
        fi
    fi
    [[ "$missing" -eq 0 ]]
}

# assert_health_ready <json> [want_cache] [want_persist]
assert_health_ready() {
    local f="$1" wc="${2:-redis}" wp="${3:-postgres}"
    [[ -f "$f" ]] || { echo "  [FAIL] health JSON 不存在: $f"; return 1; }
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$f" "$wc" "$wp" <<'PY'
import json, sys
f, wc, wp = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    d = json.load(open(f))
except Exception as e:
    print("  [FAIL] health JSON 解析失败:", e); sys.exit(1)
ok = True
def chk(c, m):
    global ok
    print(("  [PASS] " if c else "  [FAIL] ") + m); ok = ok and c
ch = d.get("checks", {})
chk(d.get("status") == "ready", "status=ready")
chk(ch.get("cache_store", {}).get("backend") == wc, "cache_store.backend=%s" % wc)
chk(ch.get("persistent_store", {}).get("backend") == wp, "persistent_store.backend=%s" % wp)
chk(ch.get("cache_store", {}).get("healthy") is True, "cache_store.healthy")
chk(ch.get("persistent_store", {}).get("healthy") is True, "persistent_store.healthy")
sys.exit(0 if ok else 1)
PY
        return $?
    fi
    # grep fallback（无 python3）
    local missing=0
    grep -Fq '"status":"ready"' "$f" || { echo "  [FAIL] status!=ready"; missing=1; }
    grep -Eq "\"cache_store\":\{[^}]*\"backend\":\"${wc}\"" "$f" || { echo "  [FAIL] cache backend!=${wc}"; missing=1; }
    grep -Eq "\"persistent_store\":\{[^}]*\"backend\":\"${wp}\"" "$f" || { echo "  [FAIL] persistent backend!=${wp}"; missing=1; }
    [[ "$missing" -eq 0 ]]
}

# upstream_payload <model>: 生成真实上游 smoke 请求体。
# 默认模型须与 config/models.yaml 的生产可用模型对齐，避免探测不存在模型后误入 fallback。
upstream_payload() {
    local model="$1"
    printf '{"model":"%s","messages":[{"role":"user","content":"ping"}]}' "$model"
}

# detect_startup_problem <app_log>: 检视 smoke 自起实例的日志，命中已知致命模式时
# echo 一句人类可读原因（供 FAIL 定位），无问题则 echo 空。纯函数，便于单测。
# 目的：自起实例若 strict-abort/端口占用而死掉，要清晰报因，而非默默串到陌生实例。
detect_startup_problem() {
    local f="$1"
    [[ -f "$f" ]] || { echo "启动日志缺失（进程可能未拉起 / 二进制不可执行）"; return 0; }
    if grep -Eq "strict_backends=true -> refusing to start|Startup aborted" "$f"; then
        echo "strict 模式拒绝启动：配置请求的后端未真实激活（常见：未 export POSTGRES_URL/REDIS_PASSWORD 等环境变量到本 shell）"
        return 0
    fi
    if grep -Eiq "address already in use|bind.*in use|Failed to listen" "$f"; then
        echo "端口被占用：可能已有实例在该端口运行（请先停止现有实例，或用 --no-start 指向它）"
        return 0
    fi
    if ! grep -Fq "Listening on" "$f"; then
        echo "未见 'Listening on'：实例未完成启动（见 app.log 末尾）"
        return 0
    fi
    echo ""
}

# ============================================================
# 主流程区（仅直接执行时运行；被 source 时不执行）
# ============================================================

SMOKE_PASS=0
SMOKE_FAIL=0
SMOKE_APP_PID=""
_record() { # <ok 0/1> <desc>
    if [[ "$1" -eq 0 ]]; then echo "[PASS] $2"; SMOKE_PASS=$((SMOKE_PASS + 1));
    else echo "[FAIL] $2"; SMOKE_FAIL=$((SMOKE_FAIL + 1)); fi
}

main() {
    local mode="real" with_upstream=0 host="127.0.0.1" port="8080"
    local api_key="${AEGISGATE_API_KEY:-}" config="config/aegisgate.prod.yaml"
    local upstream_model="${SMOKE_UPSTREAM_MODEL:-deepseek-chat}"
    local binary="build/src/aegisgate" cmake_log="" out=".smoke-out" no_start=0

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --mode) mode="$2"; shift 2 ;;
            --with-upstream) with_upstream=1; shift ;;
            --host) host="$2"; shift 2 ;;
            --port) port="$2"; shift 2 ;;
            --api-key) api_key="$2"; shift 2 ;;
            --upstream-model) upstream_model="$2"; shift 2 ;;
            --config) config="$2"; shift 2 ;;
            --binary) binary="$2"; shift 2 ;;
            --cmake-log) cmake_log="$2"; shift 2 ;;
            --out) out="$2"; shift 2 ;;
            --no-start) no_start=1; shift ;;
            -h|--help) grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; return 0 ;;
            *) echo "未知参数: $1" >&2; return 2 ;;
        esac
    done

    umask 077
    mkdir -p "$out"
    local app_log="$out/app.log" health_json="$out/health.json"
    # shellcheck disable=SC2317
    cleanup() { [[ -n "${SMOKE_APP_PID:-}" ]] && kill "$SMOKE_APP_PID" 2>/dev/null; return 0; }
    trap cleanup EXIT

    echo "==> smoke-prod (mode=$mode with_upstream=$with_upstream out=$out)"

    # 1) 构建期能力摘要
    if [[ -n "$cmake_log" ]]; then
        if assert_capability_summary "$cmake_log"; then _record 0 "构建能力摘要五项 ON"; else _record 1 "构建能力摘要五项 ON"; fi
    else
        echo "  [WARN] 未提供 --cmake-log，跳过构建能力摘要断言"
    fi

    # 2) 启动进程（除非 --no-start）
    local app_up=0
    if [[ "$no_start" -eq 1 ]]; then
        app_up=1   # 断言已在运行的外部实例（调用方保证就绪）
    else
        # 端口预检：若该端口已有实例在服务，smoke 自起会与之冲突，且后续 /health
        # 会误连陌生实例 → 直接拒绝，让用户先停现有实例或改用 --no-start。
        if command -v curl >/dev/null 2>&1 && \
           curl -fsS "http://${host}:${port}/health/ready" -o /dev/null 2>/dev/null; then
            _record 1 "端口 ${host}:${port} 空闲（自起避免实例冲突）—— 已被占用：请先停止现有实例或用 --no-start"
        else
            [[ -x "$binary" ]] || _record 1 "二进制可执行: $binary"
            if [[ -x "$binary" ]]; then
                AEGISGATE_PORT="$port" "$binary" "$config" >"$app_log" 2>&1 &
                SMOKE_APP_PID=$!
                local waited=0
                while [[ "$waited" -lt 30 ]]; do
                    grep -Fq "Listening on" "$app_log" 2>/dev/null && { app_up=1; break; }
                    kill -0 "$SMOKE_APP_PID" 2>/dev/null || break
                    sleep 1; waited=$((waited + 1))
                done
                if [[ "$app_up" -eq 0 ]]; then
                    local why; why="$(detect_startup_problem "$app_log")"
                    _record 1 "smoke 自起实例就绪 —— 未就绪：${why:-未知原因}"
                fi
            fi
        fi
    fi

    # 3) 启动日志后端（ci 模式接受 guard pass-through 编译证明）
    #    仅当 smoke 自起实例就绪时校验其 app.log；--no-start 无自起日志可查 → 跳过。
    local guard_mode="active"; [[ "$mode" == "ci" ]] && guard_mode="any"
    if [[ "$no_start" -eq 1 ]]; then
        echo "  [SKIP] --no-start：外部实例启动日志不可得，跳过启动日志断言"
    elif [[ "$app_up" -eq 1 ]]; then
        if assert_startup_log "$app_log" "$guard_mode"; then _record 0 "后端启动日志生效(guard=$guard_mode)"; else _record 1 "后端启动日志生效(guard=$guard_mode)"; fi
    else
        _record 1 "后端启动日志生效(guard=$guard_mode) —— 实例未就绪，见上方原因"
    fi

    # 4) /health/ready + /metrics —— 仅在「我方实例就绪」时查，避免误连陌生实例
    if [[ "$app_up" -ne 1 ]]; then
        echo "  [SKIP] 实例未就绪，跳过 /health/ready 与 /metrics（避免误连 :${port} 上的陌生实例）"
    elif command -v curl >/dev/null 2>&1; then
        curl -fsS "http://${host}:${port}/health/ready" -o "$health_json" 2>/dev/null || true
        if assert_health_ready "$health_json"; then _record 0 "/health/ready redis+postgres ready"; else _record 1 "/health/ready redis+postgres ready"; fi
        # 5) /metrics 存活（端点需 Bearer 鉴权；counter 启动即暴露=0，与请求数无关）
        if [[ -z "$api_key" ]]; then
            _record 1 "/metrics 暴露 aegisgate_requests_total —— 缺 AEGISGATE_API_KEY/--api-key（/metrics 需鉴权）"
        elif curl -fsS -H "Authorization: Bearer ${api_key}" \
                 "http://${host}:${port}/metrics" 2>/dev/null | grep -Fq "aegisgate_requests_total"; then
            _record 0 "/metrics 暴露 aegisgate_requests_total"
        else
            _record 1 "/metrics 暴露 aegisgate_requests_total"
        fi
    else
        _record 1 "curl 可用（health/metrics 断言所需）"
    fi

    # 6) 真实上游链路（可选）
    if [[ "$with_upstream" -eq 1 ]]; then
        local code
        code="$(curl -s -o /dev/null -w '%{http_code}' \
            -H "Authorization: Bearer ${api_key}" -H "Content-Type: application/json" \
            -d "$(upstream_payload "$upstream_model")" \
            "http://${host}:${port}/v1/chat/completions" 2>/dev/null || echo 000)"
        if [[ "$code" == "200" ]]; then _record 0 "上游 /v1/chat/completions 200 (model=$upstream_model)"; else _record 1 "上游 /v1/chat/completions 200 (model=$upstream_model, got $code)"; fi
    fi

    # 7) 侧证（仅 --mode real / best-effort）
    if [[ "$mode" == "real" ]]; then
        echo "==> 侧证（real / best-effort）"
        if command -v redis-cli >/dev/null 2>&1; then
            redis-cli KEYS '*' 2>/dev/null | head -5 | redact || true
        else echo "  [INFO] redis-cli 不可用，跳过 redis 侧证"; fi
        if command -v psql >/dev/null 2>&1 && [[ -n "${POSTGRES_URL:-}" ]]; then
            psql "${POSTGRES_URL}" -c '\dt' 2>/dev/null | redact || true
        else echo "  [INFO] psql/POSTGRES_URL 不可用，跳过 pg 侧证"; fi
    fi

    # 8) 证据脱敏 + 权限收紧（SR1/SR3）
    local fcopy
    for fcopy in "$app_log" "$health_json"; do
        [[ -f "$fcopy" ]] || continue
        redact < "$fcopy" > "${fcopy}.redacted" && mv "${fcopy}.redacted" "$fcopy"
        chmod 600 "$fcopy"
    done

    echo ""
    echo "================ SMOKE 汇总 ================"
    echo "PASS=$SMOKE_PASS  FAIL=$SMOKE_FAIL  (证据: $out)"
    echo "==========================================="
    [[ "$SMOKE_FAIL" -eq 0 ]]
}

if [[ "${BASH_SOURCE[0]:-$0}" == "${0}" ]]; then
    main "$@"
fi
