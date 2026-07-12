#!/usr/bin/env bash
# TASK-W-Phase5.2-ConfigRobustness — scripts/download_model.sh 完整性校验离线单测。
#
# 背景（E6 真机实战 / 2026-06-23）：bge embedding 模型曾因下载中段损坏（全长
# 95MB、文件头合法、IR=8）导致 onnxruntime "Protobuf parsing failed" → 语义缓存
# 静默降级 HashEmbedder。download_model.sh 此前无任何下载后校验。本测试守护新增的：
#   - validate_onnx_file  零依赖结构校验（拦 HTML 错误页/LFS 指针/截断/非 ONNX 开头）
#   - expected_bge_sha     env BGE_MODEL_SHA256 优先于内置 pin
#   - verify_bge_onnx      sha256 fail-closed（pin/env 命中即强校验，不一致退出非 0）
#
# 沿用 source + BASH_SOURCE 守卫范式（参考 test_smoke_prod.sh）。
# 用法: bash tests/scripts/test_download_model_integrity.sh   退出 0=全过 1=任一失败

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DL="$ROOT_DIR/scripts/download_model.sh"

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

if [[ ! -f "$DL" ]]; then
    echo "  [FAIL] scripts/download_model.sh 不存在"
    exit 1
fi
# shellcheck source=/dev/null
source "$DL"
set +e   # 中和被 source 脚本的 set -e，由本测试显式 if/&&/|| 控制流程

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 测试用低门槛体积（避免造 1MB+ fixture），脚本读 BGE_MIN_BYTES env。
export BGE_MIN_BYTES=64

# 构造一个"结构合法"的 ONNX 桩：首字节 0x08（ir_version 字段 tag）+ 填充到门槛以上。
GOOD="$TMP/good.onnx"
printf '\x08\x08\x12\x07pytorch' > "$GOOD"
head -c 200 /dev/zero >> "$GOOD"   # 填充到 > BGE_MIN_BYTES

# ---------- validate_onnx_file ----------
echo "==> validate_onnx_file"
if validate_onnx_file "$GOOD" >/dev/null 2>&1; then
    pass "合法 ONNX 桩（0x08 开头 + 足够大）通过"
else
    fail "合法 ONNX 桩应通过"
fi

HTML="$TMP/err.html"
printf '<!DOCTYPE html><html><body>404 Not Found via proxy</body></html>' > "$HTML"
head -c 200 /dev/zero >> "$HTML"   # 即使填大，开头非 0x08 也应拒
if validate_onnx_file "$HTML" >/dev/null 2>&1; then
    fail "HTML 错误页应被拒（开头非 0x08）"
else
    pass "HTML 错误页被拒"
fi

LFS="$TMP/pointer.onnx"
printf 'version https://git-lfs.github.com/spec/v1\noid sha256:abc\nsize 95000000\n' > "$LFS"
if validate_onnx_file "$LFS" >/dev/null 2>&1; then
    fail "LFS 指针应被拒"
else
    pass "LFS 指针被拒"
fi

TINY="$TMP/tiny.onnx"
printf '\x08\x08' > "$TINY"   # 0x08 开头但远小于门槛
if validate_onnx_file "$TINY" >/dev/null 2>&1; then
    fail "截断/过小文件应被拒"
else
    pass "截断/过小文件被拒（< BGE_MIN_BYTES）"
fi

if validate_onnx_file "$TMP/nonexistent.onnx" >/dev/null 2>&1; then
    fail "不存在文件应被拒"
else
    pass "不存在文件被拒"
fi

# ---------- expected_bge_sha（env 优先于 pin）----------
echo "==> expected_bge_sha"
(
    unset BGE_MODEL_SHA256
    BGE_ONNX_SHA256_PIN="pinvalue"
    [[ "$(expected_bge_sha)" == "pinvalue" ]]
) && pass "无 env 时取内置 pin" || fail "无 env 应取内置 pin"

(
    export BGE_MODEL_SHA256="envvalue"
    BGE_ONNX_SHA256_PIN="pinvalue"
    [[ "$(expected_bge_sha)" == "envvalue" ]]
) && pass "env 覆盖内置 pin" || fail "env 应优先于内置 pin"

# ---------- verify_bge_onnx（sha256 fail-closed）----------
echo "==> verify_bge_onnx"
ACTUAL_SHA="$(sha256sum "$GOOD" | awk '{print $1}')"

(
    export BGE_MODEL_SHA256="$ACTUAL_SHA"
    verify_bge_onnx "$GOOD" >/dev/null 2>&1
) && pass "sha256 匹配 → 通过" || fail "sha256 匹配应通过"

(
    export BGE_MODEL_SHA256="0000000000000000000000000000000000000000000000000000000000000000"
    verify_bge_onnx "$GOOD" >/dev/null 2>&1
) && fail "sha256 不匹配应 fail-closed（非 0 退出）" || pass "sha256 不匹配 → fail-closed 拒绝"

(
    unset BGE_MODEL_SHA256
    BGE_ONNX_SHA256_PIN=""
    # 无 pin/env：结构校验过即放行（不阻断既有用法），但应提示未固化
    out="$(verify_bge_onnx "$GOOD" 2>&1)"
    [[ $? -eq 0 ]] && echo "$out" | grep -q "未固化"
) && pass "无 pin → 结构通过 + 提示未固化" || fail "无 pin 应结构通过且提示未固化"

(
    export BGE_MODEL_SHA256="$ACTUAL_SHA"
    # 结构非法（HTML）即使 env 有 sha 也应先被结构校验拦下
    verify_bge_onnx "$HTML" >/dev/null 2>&1
) && fail "结构非法应被拒（即便给了 sha）" || pass "结构校验先于 sha，HTML 被拒"

echo ""
echo "================================="
echo "通过: $PASS / 失败: $FAIL"
echo "================================="
[[ "$FAIL" -eq 0 ]]
