#!/usr/bin/env bash
set -euo pipefail

# Download the prompt-injection guard model selected in TASK-20260612-01.
#
# Usage:
#   scripts/download_guard_model.sh [output_dir] [--allow-unverified]
#
# Optional env:
#   HF_ENDPOINT=https://huggingface.co   # or a compatible mirror
#   CURL_OPTS="--proxy http://127.0.0.1:7890"
#   GUARD_MODEL_SHA256_ONNX=<hex>        # 覆盖 .onnx 期望校验值
#   GUARD_MODEL_SHA256_SPM=<hex>         # 覆盖 .spm.model 期望校验值
#
# 供应链安全（fail-closed，SR-1 / TASK-20260614-02）：
#   - 核心权重文件 (.onnx / .spm.model) 下载后用 sha256sum 与内置 pin 比对，
#     不一致即删除并退出 1。
#   - 若当前文件无内置 pin 且未经 env 覆盖：默认**拒绝下载**（不下即信任）；
#     可用 GUARD_MODEL_SHA256_* 提供期望值，或 --allow-unverified 显式放行（不推荐）。
#   - 非核心文件 (config / tokenizer json) 仅做存在性校验。
#
# 退出码: 0 成功 / 1 校验或下载失败 / 2 参数错误

OUTPUT_DIR="models/guard"
ALLOW_UNVERIFIED=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --allow-unverified) ALLOW_UNVERIFIED=1; shift ;;
        -h|--help) sed -n '3,24p' "$0"; exit 0 ;;
        -*) echo "未知参数: $1" >&2; exit 2 ;;
        *) OUTPUT_DIR="$1"; shift ;;
    esac
done

MODEL_REPO="protectai/deberta-v3-base-prompt-injection-v2"
MODEL_BASENAME="deberta-v3-base-prompt-injection-v2"
HF_ENDPOINT="${HF_ENDPOINT:-https://huggingface.co}"
BASE_URL="${HF_ENDPOINT}/${MODEL_REPO}/resolve/main/onnx"

# ---- sha256 pin 表（fail-closed，SR-1）-------------------------------------
# 来源：本机 (Linux WSL2) 于 2026-06-12 从上游 HF 下载后用 sha256sum 计算回填。
# 若上游重新发布模型，哈希会变 → 用 GUARD_MODEL_SHA256_* env 覆盖，
# 或 --allow-unverified 逃生口（不推荐）。
declare -A SHA256_PINS=(
    ["${MODEL_BASENAME}.onnx"]="f0ea7f239f765aedbde7c9e163a7cb38a79c5b8853d3f76db5152172047b228c"
    ["${MODEL_BASENAME}.spm.model"]="c679fbf93643d19aab7ee10c0b99e460bdbc02fedf34b92b05af343b4af586fd"
)
# env 覆盖映射：核心文件 -> 对应 env 变量名
declare -A SHA256_ENV=(
    ["${MODEL_BASENAME}.onnx"]="GUARD_MODEL_SHA256_ONNX"
    ["${MODEL_BASENAME}.spm.model"]="GUARD_MODEL_SHA256_SPM"
)

mkdir -p "$OUTPUT_DIR"

# 返回核心文件的期望 sha256（env 覆盖优先于内置 pin），非核心文件返回空串。
expected_sha_for() {
    local name="$1"
    local env_var="${SHA256_ENV[$name]:-}"
    if [[ -n "$env_var" ]]; then
        local env_val="${!env_var:-}"
        if [[ -n "$env_val" ]]; then
            echo "$env_val"
            return
        fi
    fi
    echo "${SHA256_PINS[$name]:-}"
}

# 是否核心权重文件（参与 fail-closed 校验）
is_core_file() {
    [[ -n "${SHA256_ENV[$1]:-}" ]]
}

download_file() {
    local remote_name="$1"
    local local_name="$2"
    local url="${BASE_URL}/${remote_name}"
    local target="${OUTPUT_DIR}/${local_name}"
    local tmp="${target}.tmp"

    local expected=""
    if is_core_file "$local_name"; then
        expected="$(expected_sha_for "$local_name")"
        # fail-closed: 核心文件无 pin 且未放行 → 下载前即拒绝
        if [[ -z "$expected" && "$ALLOW_UNVERIFIED" -ne 1 ]]; then
            echo "ERROR: ${local_name} 无内置 sha256 pin（供应链 fail-closed）。" >&2
            echo "       请用 ${SHA256_ENV[$local_name]}=<hex> 提供期望值，" >&2
            echo "       或显式 --allow-unverified 放行（不推荐）。" >&2
            exit 1
        fi
    fi

    if [[ -s "$target" ]]; then
        # 已存在的核心文件也要校验（防止被替换/损坏）
        if is_core_file "$local_name" && [[ -n "$expected" ]]; then
            local cur_sha
            cur_sha="$(sha256sum "$target" | awk '{print $1}')"
            if [[ "$cur_sha" != "$expected" ]]; then
                echo "ERROR: 现有 ${target} sha256 与 pin 不一致！" >&2
                echo "  期望: $expected" >&2
                echo "  实际: $cur_sha" >&2
                exit 1
            fi
        fi
        echo "[skip] ${target} already exists"
        return 0
    fi

    echo "[download] ${remote_name} -> ${target}"
    # shellcheck disable=SC2086 # intentional word splitting for CURL_OPTS
    # --retry-all-errors：让 curl 也对 TLS 握手等传输层错误（如 exit 35
    # "unexpected eof while reading"，连 HF 时常见的瞬时抖动）自动重试，
    # 而不仅是 HTTP 5xx；配合 -C - 断点续传，大文件中断后可接续。
    curl --http1.1 -fSL --retry 5 --retry-delay 2 --retry-all-errors --connect-timeout 30 \
        -C - ${CURL_OPTS:-} "$url" -o "$tmp"
    test -s "$tmp"

    # 核心文件 fail-closed 校验：不一致即删除临时文件并退出
    if is_core_file "$local_name"; then
        local actual_sha
        actual_sha="$(sha256sum "$tmp" | awk '{print $1}')"
        if [[ -n "$expected" ]]; then
            if [[ "$actual_sha" != "$expected" ]]; then
                echo "ERROR: ${local_name} sha256 校验失败！" >&2
                echo "  期望: $expected" >&2
                echo "  实际: $actual_sha" >&2
                rm -f "$tmp"
                exit 1
            fi
            echo "  sha256 校验通过: $actual_sha"
        else
            echo "  ⚠ 未校验（--allow-unverified）。实际 sha256: $actual_sha"
            echo "    建议把该值回填到 SHA256_PINS[$local_name] 以固化供应链。"
        fi
    fi

    mv "$tmp" "$target"
}

echo "=== AegisGate Guard Model Downloader ==="
echo "Model: ${MODEL_REPO}"
echo "Endpoint: ${HF_ENDPOINT}"
echo "Output: ${OUTPUT_DIR}"
echo ""

download_file "model.onnx" "${MODEL_BASENAME}.onnx"
download_file "spm.model" "${MODEL_BASENAME}.spm.model"
download_file "config.json" "${MODEL_BASENAME}.config.json"
download_file "tokenizer_config.json" "${MODEL_BASENAME}.tokenizer_config.json"
download_file "special_tokens_map.json" "${MODEL_BASENAME}.special_tokens_map.json"

echo ""
echo "=== Verification ==="
for f in \
    "${MODEL_BASENAME}.onnx" \
    "${MODEL_BASENAME}.spm.model" \
    "${MODEL_BASENAME}.config.json" \
    "${MODEL_BASENAME}.tokenizer_config.json" \
    "${MODEL_BASENAME}.special_tokens_map.json"; do
    if [[ ! -s "${OUTPUT_DIR}/${f}" ]]; then
        echo "missing: ${OUTPUT_DIR}/${f}" >&2
        exit 1
    fi
    echo "present: ${OUTPUT_DIR}/${f}"
done

echo ""
echo "Configure in config/aegisgate.yaml:"
echo "security:"
echo "  guard_model:"
echo "    enabled: true"
echo "    model_path: \"${OUTPUT_DIR}/${MODEL_BASENAME}.onnx\""
echo "    spm_model_path: \"${OUTPUT_DIR}/${MODEL_BASENAME}.spm.model\""
echo "    threshold: 0.5"
echo "    fail_policy: open"
