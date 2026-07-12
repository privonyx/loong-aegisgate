#!/usr/bin/env bash
set -euo pipefail

# Download BGE-small-zh-v1.5 model for AegisGate semantic cache
# Usage: ./scripts/download_model.sh [output_dir] [python|curl|auto]
#
# 供应链 / 完整性（TASK-W-Phase5.2-ConfigRobustness，E6 实战回填）：
#   下载后对 .onnx 做两层校验，避免"全长但损坏"的文件静默落盘导致
#   onnxruntime "Protobuf parsing failed" → 语义缓存悄悄降级 HashEmbedder：
#     1. validate_onnx_file  零依赖结构校验（拦 HTML 错误页 / LFS 指针 /
#        截断过小 / 非 ONNX 开头）；中段损坏仅 sha256 能查。
#     2. sha256 fail-closed   若 BGE_MODEL_SHA256 env 或内置 BGE_ONNX_SHA256_PIN
#        命中则强校验，不一致即删除并退出 1；无 pin 时仅结构校验 + 提示固化。
#   curl(预导出 onnx) 路径参与 sha256 校验；python 本地导出哈希因 torch/optimum
#   版本而异，故仅做结构校验、不套固定 pin。

OUTPUT_DIR="${1:-models}"
MODEL_NAME="BAAI/bge-small-zh-v1.5"
# 官方仓库只有 PyTorch 权重，预导出的 ONNX 由社区仓库提供
ONNX_REPO="Xenova/bge-small-zh-v1.5"
ONNX_REMOTE_PATH="onnx/model.onnx"
ONNX_FILE="model.onnx"
VOCAB_FILE="vocab.txt"
ONNX_LOCAL="bge-small-zh-v1.5.onnx"

HF_MIRROR="${HF_ENDPOINT:-https://huggingface.co}"

# ---- sha256 pin（fail-closed / SR-1）---------------------------------------
# 内置 pin 默认空：上游 Xenova 预导出 onnx 若稳定，可在本机用可信网络下载、
# 确认能被 onnxruntime 加载后，用 `sha256sum` 计算回填于此固化供应链。
# 运行时可用 BGE_MODEL_SHA256=<hex> 覆盖（优先于内置 pin）。
BGE_ONNX_SHA256_PIN="${BGE_ONNX_SHA256_PIN:-}"

# 返回 bge onnx 的期望 sha256：env BGE_MODEL_SHA256 优先于内置 pin；都没有则空串。
expected_bge_sha() {
    if [[ -n "${BGE_MODEL_SHA256:-}" ]]; then
        echo "${BGE_MODEL_SHA256}"
        return
    fi
    echo "${BGE_ONNX_SHA256_PIN}"
}

# 零依赖结构校验：拦 HTML 错误页 / LFS 指针 / 截断过小 / 非 ONNX 开头。
# 返回 0=结构合法，非 0=非法（并 stderr 说明）。BGE_MIN_BYTES 可调（测试用）。
validate_onnx_file() {
    local f="$1"
    local min_bytes="${BGE_MIN_BYTES:-1048576}"   # ONNX 至少 1MB（bge-small ~95MB）
    if [[ ! -s "$f" ]]; then
        echo "ERROR: $f 不存在或为空" >&2
        return 1
    fi
    local sz
    sz="$(stat -c%s "$f")"
    if [[ -z "$sz" || "$sz" -lt "$min_bytes" ]]; then
        echo "ERROR: $f 仅 ${sz:-0} 字节（< ${min_bytes}）→ 疑似截断/错误页/LFS 指针" >&2
        return 1
    fi
    # ONNX ModelProto 以字段1 ir_version（tag 0x08）开头；HTML/文本/LFS 指针不会。
    local first
    first="$(head -c 1 "$f" | od -An -tu1 | tr -d ' ')"
    if [[ "$first" != "8" ]]; then
        echo "ERROR: $f 开头字节非 0x08（ONNX ir_version 字段）→ 非合法 ONNX" >&2
        echo "       （常见原因：下载到 HTML 错误页 / Git LFS 指针文件）" >&2
        return 1
    fi
    return 0
}

# 对 bge onnx 做结构校验 + sha256 fail-closed。返回 0=可信，非 0=拒绝。
verify_bge_onnx() {
    local f="$1"
    validate_onnx_file "$f" || return 1
    local expected actual
    expected="$(expected_bge_sha)"
    actual="$(sha256sum "$f" | awk '{print $1}')"
    if [[ -n "$expected" ]]; then
        if [[ "$actual" != "$expected" ]]; then
            echo "ERROR: $f sha256 校验失败（供应链 fail-closed）" >&2
            echo "  期望: $expected" >&2
            echo "  实际: $actual" >&2
            return 1
        fi
        echo "  sha256 校验通过: $actual"
    else
        echo "  ⚠ 未固化 sha256 pin（仅结构校验通过）。实际 sha256: $actual"
        echo "    确认该模型能被 onnxruntime 加载后，可设 BGE_ONNX_SHA256_PIN 固化供应链。"
    fi
    return 0
}

download_with_python() {
    echo "[1/3] Installing optimum for ONNX export..."
    pip3 install -q optimum[onnxruntime] transformers torch --user 2>/dev/null || true

    echo "[2/3] Exporting model to ONNX..."
    python3 -c "
from optimum.onnxruntime import ORTModelForFeatureExtraction
from transformers import AutoTokenizer
import shutil, os

model = ORTModelForFeatureExtraction.from_pretrained('$MODEL_NAME', export=True)
model.save_pretrained('$OUTPUT_DIR/tmp_export')

# Move ONNX model
src_onnx = os.path.join('$OUTPUT_DIR/tmp_export', '$ONNX_FILE')
if os.path.exists(src_onnx):
    shutil.move(src_onnx, os.path.join('$OUTPUT_DIR', '$ONNX_LOCAL'))

tokenizer = AutoTokenizer.from_pretrained('$MODEL_NAME')
tokenizer.save_vocabulary('$OUTPUT_DIR')

shutil.rmtree('$OUTPUT_DIR/tmp_export', ignore_errors=True)
print('Done!')
"
    # 本地导出哈希随 torch/optimum 版本而异 → 仅结构校验（不套固定 pin）。
    echo "[3/3] 校验导出的 ONNX 结构完整性..."
    validate_onnx_file "$OUTPUT_DIR/$ONNX_LOCAL" || {
        echo "ERROR: 本地导出的 ONNX 结构校验失败" >&2
        exit 1
    }
    echo "[3/3] Model exported successfully!"
}

download_with_curl() {
    BASE_URL="$HF_MIRROR/$MODEL_NAME/resolve/main"

    # --retry-all-errors + -C -：对 TLS 握手等瞬时传输错误自动重试并断点续传，
    # 缓解连 HF 不稳导致的 "unexpected eof while reading"（与 guard 下载脚本一致）。
    CURL_RETRY=(--retry 5 --retry-delay 2 --retry-all-errors --connect-timeout 30 -C -)

    echo "[1/2] Downloading vocab.txt..."
    curl -fSL "${CURL_RETRY[@]}" "$BASE_URL/vocab.txt" -o "$OUTPUT_DIR/vocab.txt"

    echo "[2/2] Downloading ONNX model from $ONNX_REPO..."
    local tmp="$OUTPUT_DIR/$ONNX_LOCAL.tmp"
    curl -fSL "${CURL_RETRY[@]}" "$HF_MIRROR/$ONNX_REPO/resolve/main/$ONNX_REMOTE_PATH" \
         -o "$tmp" || {
        echo ""
        echo "ONNX download failed. You can either retry with a mirror:"
        echo "  HF_ENDPOINT=https://hf-mirror.com $0 $OUTPUT_DIR curl"
        echo "or use Python export:"
        echo "  pip install optimum[onnxruntime] transformers torch"
        echo "  python -c \"from optimum.onnxruntime import ORTModelForFeatureExtraction; \\"
        echo "    m = ORTModelForFeatureExtraction.from_pretrained('$MODEL_NAME', export=True); \\"
        echo "    m.save_pretrained('$OUTPUT_DIR')\""
        rm -f "$tmp"
        exit 1
    }

    # fail-closed：结构 + sha256 校验未过则删除临时文件，绝不落盘损坏模型。
    echo "[2/2] 校验下载的 ONNX（结构 + sha256）..."
    if ! verify_bge_onnx "$tmp"; then
        rm -f "$tmp"
        echo "ERROR: 下载的 ONNX 未通过完整性校验，已删除。请重试或换镜像。" >&2
        exit 1
    fi
    mv "$tmp" "$OUTPUT_DIR/$ONNX_LOCAL"
}

run_download_model() {
    mkdir -p "$OUTPUT_DIR"

    echo "=== AegisGate Model Downloader ==="
    echo "Model: $MODEL_NAME"
    echo "Mirror: $HF_MIRROR"
    echo "Output: $OUTPUT_DIR"
    echo ""

    # Check for required tools
    if ! command -v python3 &>/dev/null && ! command -v curl &>/dev/null; then
        echo "Error: python3 or curl is required"
        exit 1
    fi

    local mode="${2:-auto}"
    case "$mode" in
        python)
            download_with_python
            ;;
        curl)
            download_with_curl
            ;;
        auto)
            if command -v python3 &>/dev/null && python3 -c "import optimum" 2>/dev/null; then
                download_with_python
            else
                download_with_curl
            fi
            ;;
        *)
            echo "Usage: $0 [output_dir] [python|curl|auto]"
            exit 1
            ;;
    esac

    echo ""
    echo "=== Verification ==="
    [ -f "$OUTPUT_DIR/$ONNX_LOCAL" ] && echo "✓ ONNX model: $OUTPUT_DIR/$ONNX_LOCAL" || echo "✗ ONNX model missing"
    [ -f "$OUTPUT_DIR/$VOCAB_FILE" ] && echo "✓ Vocabulary: $OUTPUT_DIR/$VOCAB_FILE" || echo "✗ Vocabulary missing"
    echo ""
    echo "Configure in aegisgate.yaml:"
    echo "  embedding:"
    echo "    model_path: \"$OUTPUT_DIR/$ONNX_LOCAL\""
    echo "    vocab_path: \"$OUTPUT_DIR/$VOCAB_FILE\""
}

# 仅作为脚本直接执行时才下载；被 source（如离线单测）时只暴露函数。
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    run_download_model "$@"
fi
