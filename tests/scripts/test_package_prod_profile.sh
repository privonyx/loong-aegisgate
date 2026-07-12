#!/usr/bin/env bash
# TASK-20260622-01 E3 — package.sh 生产档位对齐离线单测。
#
# G2：分发包默认只携带 community 模板（memory/sqlite），tar 部署掉回非生产档位。
# 本测试断言：
#   1. package.sh 把 config/aegisgate.prod.yaml 作为 aegisgate.prod.yaml.example 纳入包
#   2. 内嵌 start.sh 支持 --profile prod / AEGISGATE_PROFILE=prod → 选用 prod 模板并启动
#   3. 默认（无 profile）仍选 community 模板（向后兼容）
#
# 范式：从 package.sh 提取内嵌 start.sh，配假 aegisgate（回显 argv）在隔离目录跑。
# 用法: bash tests/scripts/test_package_prod_profile.sh   退出 0=全过 1=任一失败

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PACKAGE_SH="$ROOT_DIR/scripts/package.sh"

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

[[ -f "$PACKAGE_SH" ]] || { echo "[FATAL] package.sh 不存在"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ---------- 1. package.sh 携带 prod 模板 ----------
echo "==> package.sh 携带 prod 模板"
if grep -Eq 'aegisgate\.prod\.yaml.*aegisgate\.prod\.yaml\.example|config/aegisgate\.prod\.yaml"? "\$DIST_DIR/config/aegisgate\.prod\.yaml\.example"' "$PACKAGE_SH"; then
    pass "package.sh 拷贝 config/aegisgate.prod.yaml → *.prod.yaml.example"
else
    fail "package.sh 未携带 prod 模板"
fi

# ---------- 提取内嵌 start.sh ----------
sed -n "/<< 'STARTUP'/,/^STARTUP\$/p" "$PACKAGE_SH" | sed '1d;$d' > "$TMP/start.sh"
chmod +x "$TMP/start.sh"
if [[ ! -s "$TMP/start.sh" ]]; then
    echo "  [FAIL] 无法从 package.sh 提取内嵌 start.sh"
    echo "[SUMMARY] PASS=$PASS FAIL=$((FAIL + 1))"
    exit 1
fi

# 公共：在隔离工作目录用假 aegisgate 跑 start.sh，回显被启动的 config 路径
run_start() { # <workdir> <extra env / args...>
    local work="$1"; shift
    mkdir -p "$work/config"
    printf 'CONFIG_KIND: community\n' > "$work/config/aegisgate.yaml.example"
    printf 'models: community\n' > "$work/config/models.yaml.example"
    printf 'CONFIG_KIND: prod\n' > "$work/config/aegisgate.prod.yaml.example"
    cat > "$work/aegisgate" <<'FAKE'
#!/usr/bin/env bash
echo "FAKE_LAUNCH_CONFIG=$1"
exit 0
FAKE
    chmod +x "$work/aegisgate"
    cp "$TMP/start.sh" "$work/start.sh"
    # 用 env 传递，确保从 "$@" 展开的 NAME=VALUE 被当作环境赋值（而非命令）
    ( cd "$work" && env AEGISGATE_API_KEY=test "$@" bash start.sh -f 2>&1 )
}

# ---------- 2. 默认（community）----------
echo "==> 默认 profile = community"
out="$(run_start "$TMP/w_default")"
if printf '%s' "$out" | grep -Fq "FAKE_LAUNCH_CONFIG=config/aegisgate.yaml"; then
    pass "默认启动 config/aegisgate.yaml（向后兼容）"
else
    fail "默认未启动 community 配置: $(printf '%s' "$out" | grep -F FAKE_LAUNCH_CONFIG || echo '无启动行')"
fi
if [[ -f "$TMP/w_default/config/aegisgate.yaml" ]]; then
    pass "默认 seeding 生成 config/aegisgate.yaml"
else
    fail "默认未生成 community 真实配置"
fi

# ---------- 3. AEGISGATE_PROFILE=prod ----------
echo "==> AEGISGATE_PROFILE=prod"
out="$(run_start "$TMP/w_env" AEGISGATE_PROFILE=prod)"
if printf '%s' "$out" | grep -Fq "FAKE_LAUNCH_CONFIG=config/aegisgate.prod.yaml"; then
    pass "env profile=prod 启动 config/aegisgate.prod.yaml"
else
    fail "env profile=prod 未选用 prod 配置: $(printf '%s' "$out" | grep -F FAKE_LAUNCH_CONFIG || echo '无启动行')"
fi
if [[ -f "$TMP/w_env/config/aegisgate.prod.yaml" ]]; then
    pass "prod profile seeding 生成 config/aegisgate.prod.yaml"
else
    fail "prod profile 未生成 prod 真实配置"
fi

# ---------- 4. --profile prod 旗标 ----------
echo "==> --profile prod 旗标"
# 注意：run_start 末尾固定追加 -f；这里把 --profile prod 作为前置参数传入
mkdir -p "$TMP/w_flag/config"
printf 'CONFIG_KIND: community\n' > "$TMP/w_flag/config/aegisgate.yaml.example"
printf 'models: community\n' > "$TMP/w_flag/config/models.yaml.example"
printf 'CONFIG_KIND: prod\n' > "$TMP/w_flag/config/aegisgate.prod.yaml.example"
cat > "$TMP/w_flag/aegisgate" <<'FAKE'
#!/usr/bin/env bash
echo "FAKE_LAUNCH_CONFIG=$1"
exit 0
FAKE
chmod +x "$TMP/w_flag/aegisgate"
cp "$TMP/start.sh" "$TMP/w_flag/start.sh"
out="$( cd "$TMP/w_flag" && AEGISGATE_API_KEY=test bash start.sh --profile prod -f 2>&1 )"
if printf '%s' "$out" | grep -Fq "FAKE_LAUNCH_CONFIG=config/aegisgate.prod.yaml"; then
    pass "--profile prod 启动 config/aegisgate.prod.yaml"
else
    fail "--profile prod 未选用 prod 配置: $(printf '%s' "$out" | grep -F FAKE_LAUNCH_CONFIG || echo '无启动行')"
fi

echo ""
echo "================================="
echo "通过: $PASS / 失败: $FAIL"
echo "================================="
[[ "$FAIL" -eq 0 ]]
