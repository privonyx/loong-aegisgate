#!/usr/bin/env bash
set -euo pipefail

# scripts/fetch-onnxruntime.sh
#
# 按平台下载 Microsoft 官方 ONNX Runtime 预编译包到 third_party/，供
# cmake/Dependencies.cmake 的 A3 「平台默认目录」分支命中
# （third_party/onnxruntime-<os>-<arch>-<ver>）。
#
# 设计：决策 A3 / memory-bank/creative/creative-onnx-cross-platform-acquisition.md
#
# 供应链安全（fail-closed）：
#   - 下载后用 sha256sum 计算并与内置 pin 比对，不一致即删包并退出。
#   - 若当前平台无内置 pin，默认**拒绝**（不下载即信任）；可通过
#     ONNXRUNTIME_SHA256=<hex> 显式提供，或 --allow-unverified 显式放行（不推荐）。
#
# 用法:
#   scripts/fetch-onnxruntime.sh [--force] [--version X.Y.Z] [--allow-unverified]
#
# 环境变量:
#   ONNXRUNTIME_SHA256   覆盖/提供当前平台包的期望 sha256（十六进制）
#
# 退出码: 0 成功 / 1 校验或下载失败 / 2 参数或平台不支持

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION="1.24.2"
FORCE=0
ALLOW_UNVERIFIED=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force)            FORCE=1; shift ;;
        --version)          VERSION="$2"; shift 2 ;;
        --allow-unverified) ALLOW_UNVERIFIED=1; shift ;;
        -h|--help)
            sed -n '3,28p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

# ---- 平台 → 包名 ------------------------------------------------------------
RAW_ARCH="$(uname -m)"
case "$RAW_ARCH" in
    arm64|aarch64) ARCH="arm64" ;;
    x86_64|amd64)  ARCH="x64" ;;
    *) echo "不支持的架构: $RAW_ARCH" >&2; exit 2 ;;
esac

# Microsoft release 资产命名: onnxruntime-linux-{x64|arm64}-<ver>.tgz
case "$(uname -s)" in
    Linux) ASSET_OS="linux" ;;
    *) echo "不支持的系统: $(uname -s)（已移除 macOS 适配，仅支持 Linux）" >&2; exit 2 ;;
esac

PKG_DIRNAME="onnxruntime-${ASSET_OS}-${ARCH}-${VERSION}"
ASSET="${PKG_DIRNAME}.tgz"
URL="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/${ASSET}"
DEST="${ROOT_DIR}/third_party/${PKG_DIRNAME}"

# ---- sha256 pin 表（fail-closed）-------------------------------------------
# key: <asset_os>-<arch>-<ver>。留空表示「无内置 pin」→ 默认拒绝
# （除非 ONNXRUNTIME_SHA256 或 --allow-unverified）。
declare -A SHA256_PINS=(
    # ["linux-x64-1.24.2"]="<回填>"
)
PIN_KEY="${ASSET_OS}-${ARCH}-${VERSION}"
EXPECTED_SHA="${ONNXRUNTIME_SHA256:-${SHA256_PINS[$PIN_KEY]:-}}"

echo "ONNX Runtime fetch: ${PKG_DIRNAME}"
echo "  目标目录: ${DEST}"

if [[ -d "$DEST" && "$FORCE" -ne 1 ]]; then
    echo "  已存在（用 --force 重新下载），跳过。"
    exit 0
fi

if [[ -z "$EXPECTED_SHA" && "$ALLOW_UNVERIFIED" -ne 1 ]]; then
    echo "ERROR: 平台 ${PIN_KEY} 无内置 sha256 pin（供应链 fail-closed）。" >&2
    echo "       请用 ONNXRUNTIME_SHA256=<hex> 提供期望校验值，" >&2
    echo "       或显式 --allow-unverified 放行（不推荐）。" >&2
    exit 1
fi

# ---- 下载 -------------------------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
TARBALL="$TMP/$ASSET"

echo "  下载: $URL"
if command -v curl >/dev/null 2>&1; then
    curl -fSL "$URL" -o "$TARBALL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$TARBALL" "$URL"
else
    echo "ERROR: 需要 curl 或 wget" >&2
    exit 1
fi

# ---- 校验（fail-closed）-----------------------------------------------------
ACTUAL_SHA="$(sha256sum "$TARBALL" | awk '{print $1}')"
if [[ -n "$EXPECTED_SHA" ]]; then
    if [[ "$ACTUAL_SHA" != "$EXPECTED_SHA" ]]; then
        echo "ERROR: sha256 校验失败！" >&2
        echo "  期望: $EXPECTED_SHA" >&2
        echo "  实际: $ACTUAL_SHA" >&2
        exit 1
    fi
    echo "  sha256 校验通过: $ACTUAL_SHA"
else
    echo "  ⚠ 未校验（--allow-unverified）。实际 sha256: $ACTUAL_SHA"
    echo "    建议把该值回填到本脚本 SHA256_PINS[$PIN_KEY] 以固化供应链。"
fi

# ---- 解包到 third_party -----------------------------------------------------
mkdir -p "$ROOT_DIR/third_party"
rm -rf "$DEST"
tar -xzf "$TARBALL" -C "$TMP"
# 包内顶层目录即 ${PKG_DIRNAME}
if [[ -d "$TMP/$PKG_DIRNAME" ]]; then
    mv "$TMP/$PKG_DIRNAME" "$DEST"
else
    # 兜底：找到含 lib/cmake/onnxruntime 的目录
    extracted="$(find "$TMP" -maxdepth 2 -type d -name 'onnxruntime-*' | head -1)"
    [[ -n "$extracted" ]] || { echo "ERROR: 解包后未找到 onnxruntime 目录" >&2; exit 1; }
    mv "$extracted" "$DEST"
fi

echo "  ✓ 已就绪: $DEST"
echo "    现在可运行: cmake -B build -DENABLE_GUARD_MODEL=ON ..."
