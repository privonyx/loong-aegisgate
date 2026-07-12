#!/usr/bin/env bash
# Tests for SDK publish pipeline (PyPI + npm).
#
# Modes:
#   --dry  (default)  Grep + jq + light build-check; CI on PR friendly.
#   --full            Real `python -m build` + `pip install` of local wheel +
#                     `npm pack` + import smoke. Needs network + isolated venv.
#
# Usage:
#   bash tests/scripts/test-sdk-publish.sh           # dry mode
#   bash tests/scripts/test-sdk-publish.sh --dry
#   bash tests/scripts/test-sdk-publish.sh --full
#
# Exit: 0 = all pass, 1 = any fail.
#
# SR4 关键文本（spec §6.3 + plan §3.3 锁定 — workflow / 实测 / 本断言三方共享）：
#   - twine check                  → "PASSED"
#   - npm pack --dry-run           → "package size:" + "unpacked size:"
#   - pip install <local wheel>    → "Successfully installed aegisgate-0.1.0"
#   - python -c import aegisgate   → "0.1.0"
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

PY_DIR="$ROOT_DIR/sdk/python"
NODE_DIR="$ROOT_DIR/sdk/nodejs"
WORKFLOW="$ROOT_DIR/.github/workflows/publish-sdk.yml"

# SR4 关键字面量（plan / impl / test 共享同一字符串 - N2 教训）
EXPECTED_TWINE_OK="PASSED"
EXPECTED_NPM_PACK_SIZE="package size:"
EXPECTED_NPM_PACK_UNPACKED="unpacked size:"
EXPECTED_INSTALL_OK="Successfully installed aegisgate-0.1.0"
EXPECTED_IMPORT_VERSION="0.1.0"

MODE="dry"
case "${1:-}" in
    --dry|"") MODE="dry" ;;
    --full)   MODE="full" ;;
    -h|--help)
        sed -n '1,18p' "$0"
        exit 0
        ;;
    *)
        echo "[FATAL] unknown arg: $1" >&2
        exit 1
        ;;
esac

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
info() { echo "  [INFO] $1"; }
group() { echo ""; echo "=== $1 ==="; }

# ---------- Epic 1: Python metadata ----------

test_python_metadata() {
    group "test_python_metadata (Epic 1)"

    local pyproject="$PY_DIR/pyproject.toml"
    local init_py="$PY_DIR/aegisgate/__init__.py"

    [[ -f "$pyproject" ]] || { fail "pyproject.toml missing"; return; }
    [[ -f "$init_py" ]]   || { fail "aegisgate/__init__.py missing"; return; }

    if grep -qE '^version = "0\.1\.0"$' "$pyproject"; then
        pass "pyproject version = 0.1.0"
    else
        fail "pyproject version != 0.1.0 (found: $(grep -E '^version' "$pyproject" || echo 'none'))"
    fi

    if grep -qE '^license = .*Apache-2\.0' "$pyproject"; then
        pass "pyproject license aligned to Apache-2.0"
    else
        fail "pyproject license not Apache-2.0 (found: $(grep -E '^license' "$pyproject" || echo 'none'))"
    fi

    if grep -qE 'License :: OSI Approved :: Apache' "$pyproject"; then
        pass "pyproject classifier = Apache Software License"
    else
        fail "pyproject classifier still MIT (or missing Apache)"
    fi

    if grep -Fq 'privonyx/loong-aegisgate' "$pyproject"; then
        pass "pyproject Repository URL points to privonyx/loong-aegisgate"
    else
        fail "pyproject Repository URL not corrected (still 404 'loong-aegisgate/loong-aegisgate'?)"
    fi

    if grep -qE '^__version__ = "0\.1\.0"$' "$init_py"; then
        pass "aegisgate/__init__.py exposes __version__ = 0.1.0"
    else
        fail "aegisgate/__init__.py missing __version__ = 0.1.0"
    fi
}

# ---------- Epic 2: Node.js metadata ----------

test_nodejs_metadata() {
    group "test_nodejs_metadata (Epic 2)"

    local pkg="$NODE_DIR/package.json"
    [[ -f "$pkg" ]] || { fail "sdk/nodejs/package.json missing"; return; }

    if ! command -v jq >/dev/null 2>&1; then
        info "jq not available — using grep fallback"
        if grep -qE '"version"\s*:\s*"0\.1\.0"' "$pkg"; then
            pass "package.json version = 0.1.0"
        else
            fail "package.json version != 0.1.0"
        fi
        if grep -qE '"license"\s*:\s*"Apache-2\.0"' "$pkg"; then
            pass "package.json license = Apache-2.0"
        else
            fail "package.json license != Apache-2.0"
        fi
        if grep -qF 'privonyx/loong-aegisgate' "$pkg"; then
            pass "package.json repository URL points to privonyx/loong-aegisgate"
        else
            fail "package.json repository URL missing/wrong"
        fi
        return
    fi

    local version license repo_url
    version=$(jq -r '.version' "$pkg" 2>/dev/null || echo "")
    license=$(jq -r '.license' "$pkg" 2>/dev/null || echo "")
    repo_url=$(jq -r '.repository.url // .repository // ""' "$pkg" 2>/dev/null || echo "")

    if [[ "$version" == "0.1.0" ]]; then
        pass "package.json version = 0.1.0"
    else
        fail "package.json version = '$version' (expected 0.1.0)"
    fi

    if [[ "$license" == "Apache-2.0" ]]; then
        pass "package.json license = Apache-2.0"
    else
        fail "package.json license = '$license' (expected Apache-2.0)"
    fi

    if [[ "$repo_url" == *"privonyx/loong-aegisgate"* ]]; then
        pass "package.json repository URL points to privonyx/loong-aegisgate"
    else
        fail "package.json repository URL = '$repo_url' (expected to contain privonyx/loong-aegisgate)"
    fi
}

# ---------- Epic 3: SR1 / SR3 / SR4 + dry-run build ----------

test_sr1_no_token_literals() {
    # SR1: 任何形如 <npm-prefix>_<20+ alnum chars> 或 <pypi-prefix>-<20+ alnum chars>
    # 的 token 字面量都不得出现在 workflow / 测试脚本里。
    # 合法用法：secrets.NODE_AUTH_TOKEN 这种 GitHub Actions 引用。
    # A13 反向验证：本注释刻意不写实际 token 形态，避免 regex 自命中。
    group "test_sr1_no_token_literals (Epic 3 / SR1)"

    local scan_files=("$ROOT_DIR/tests/scripts/test-sdk-publish.sh")
    [[ -f "$WORKFLOW" ]] && scan_files+=("$WORKFLOW")

    # 反复模式 N1（pipefail + grep 早退）规避：临时文件中转
    local tmpf
    tmpf=$(mktemp)
    : > "$tmpf"
    grep -nE '(npm_|pypi-)[A-Za-z0-9_-]{20,}' "${scan_files[@]}" 2>/dev/null > "$tmpf" || true

    if [[ -s "$tmpf" ]]; then
        fail "SR1 token literals leaked:"
        sed 's/^/        /' "$tmpf"
    else
        pass "SR1 0 token literals in workflow + test script"
    fi
    rm -f "$tmpf"

    # 额外：workflow 文件若存在，必须不含 hardcoded `password:` / token key=value 模式
    if [[ -f "$WORKFLOW" ]]; then
        if grep -nE '(password|token)\s*:\s*[A-Za-z0-9]{8,}' "$WORKFLOW" >/dev/null 2>&1; then
            fail "SR1 hardcoded password/token in $WORKFLOW"
        else
            pass "SR1 no hardcoded password/token key=value in workflow"
        fi
    fi
}

test_sr3_tag_only_trigger() {
    # SR3: workflow on: 仅 push.tags: 'sdk-v*.*.*'。禁含 workflow_dispatch、
    # push.branches、pull_request 等会被滥用的触发器。
    group "test_sr3_tag_only_trigger (Epic 3 / SR3)"

    if [[ ! -f "$WORKFLOW" ]]; then
        fail "SR3 workflow file missing: $WORKFLOW (will be created in Epic 4)"
        return
    fi

    # 反复模式 N1 规避：grep 输出落盘后再判
    local tmpf
    tmpf=$(mktemp)
    grep -nE '^[[:space:]]*(workflow_dispatch|pull_request):' "$WORKFLOW" > "$tmpf" 2>/dev/null || true
    if [[ -s "$tmpf" ]]; then
        fail "SR3 forbidden trigger present:"
        sed 's/^/        /' "$tmpf"
    else
        pass "SR3 no workflow_dispatch / pull_request triggers"
    fi
    rm -f "$tmpf"

    # 兼容两种 YAML list 写法：
    #   tags: ['sdk-v*.*.*']                              （inline）
    #   tags:\n      - 'sdk-v*.*.*'                       （block）
    if grep -qE "['\"]sdk-v\*\.\*\.\*['\"]" "$WORKFLOW"; then
        pass "SR3 sdk-v*.*.* tag pattern present"
    else
        fail "SR3 sdk-v*.*.* tag pattern missing or wrong"
    fi

    if grep -qE "^[[:space:]]*branches:" "$WORKFLOW"; then
        fail "SR3 push.branches present (forbidden — tag-only)"
    else
        pass "SR3 no push.branches trigger"
    fi
}

test_sr4_keyword_hardcode() {
    # SR4: workflow + test 脚本必须 hard-code SR4 4 关键字面量
    # （N2 教训：plan / test / impl 共享同一字符串避免词不一致）
    group "test_sr4_keyword_hardcode (Epic 3 / SR4)"

    if [[ ! -f "$WORKFLOW" ]]; then
        fail "SR4 workflow file missing: $WORKFLOW (will be created in Epic 4)"
        return
    fi

    # Workflow 应在 twine check 步骤及 verify 步骤里出现 EXPECTED_TWINE_OK
    if grep -F "$EXPECTED_TWINE_OK" "$WORKFLOW" >/dev/null 2>&1; then
        pass "SR4 workflow contains '$EXPECTED_TWINE_OK'"
    else
        fail "SR4 workflow missing '$EXPECTED_TWINE_OK' (twine check assertion)"
    fi

    # npm pack 验证关键文本
    if grep -F "$EXPECTED_NPM_PACK_SIZE" "$WORKFLOW" >/dev/null 2>&1; then
        pass "SR4 workflow contains '$EXPECTED_NPM_PACK_SIZE'"
    else
        fail "SR4 workflow missing '$EXPECTED_NPM_PACK_SIZE' (npm pack assertion)"
    fi

    # 防 version drift：workflow 不得出现遗留 "1.0.0" 字面量
    if grep -nE '\b1\.0\.0\b' "$WORKFLOW" >/dev/null 2>&1; then
        fail "SR4 stale version '1.0.0' present in workflow (drift)"
    else
        pass "SR4 no stale '1.0.0' in workflow"
    fi
}

test_python_build_dry() {
    # Dry-run Python build：python -m build + twine check。
    # 如果 build/twine 未 install，标 INFO skip 不算 FAIL（CI 上必装）。
    group "test_python_build_dry (Epic 3)"

    if ! python3 -c "import build" 2>/dev/null; then
        info "skip — python 'build' package not installed (CI must install)"
        return
    fi
    if ! python3 -c "import twine" 2>/dev/null; then
        info "skip — python 'twine' package not installed (CI must install)"
        return
    fi

    local distdir
    distdir=$(mktemp -d)
    if (cd "$PY_DIR" && python3 -m build --outdir "$distdir" >/tmp/sdk-py-build.log 2>&1); then
        pass "python -m build produced wheel + sdist"
    else
        fail "python -m build failed (see /tmp/sdk-py-build.log)"
        rm -rf "$distdir"
        return
    fi

    if ls "$distdir"/aegisgate-0.1.0-*.whl >/dev/null 2>&1 \
       && ls "$distdir"/aegisgate-0.1.0.tar.gz >/dev/null 2>&1; then
        pass "dist artifacts named aegisgate-0.1.0-*.whl + aegisgate-0.1.0.tar.gz"
    else
        fail "dist artifacts missing or wrong version"
    fi

    if python3 -m twine check "$distdir"/* > /tmp/sdk-py-twine.log 2>&1; then
        if grep -F "$EXPECTED_TWINE_OK" /tmp/sdk-py-twine.log >/dev/null 2>&1; then
            pass "twine check output contains '$EXPECTED_TWINE_OK'"
        else
            fail "twine check did not contain '$EXPECTED_TWINE_OK' (see /tmp/sdk-py-twine.log)"
        fi
    else
        fail "twine check failed (see /tmp/sdk-py-twine.log)"
    fi

    rm -rf "$distdir"
}

test_nodejs_pack_dry() {
    # Dry-run Node pack：cd sdk/nodejs && npm pack --dry-run
    # 输出（stderr）必须含 'package size:' + 'unpacked size:'。
    group "test_nodejs_pack_dry (Epic 3)"

    if ! command -v npm >/dev/null 2>&1; then
        info "skip — npm not on PATH"
        return
    fi

    local logf=/tmp/sdk-node-pack.log
    if (cd "$NODE_DIR" && npm pack --dry-run > "$logf" 2>&1); then
        pass "npm pack --dry-run succeeded"
    else
        fail "npm pack --dry-run failed (see $logf)"
        return
    fi

    if grep -F "$EXPECTED_NPM_PACK_SIZE" "$logf" >/dev/null 2>&1; then
        pass "npm pack output contains '$EXPECTED_NPM_PACK_SIZE'"
    else
        fail "npm pack output missing '$EXPECTED_NPM_PACK_SIZE' (see $logf)"
    fi

    if grep -F "$EXPECTED_NPM_PACK_UNPACKED" "$logf" >/dev/null 2>&1; then
        pass "npm pack output contains '$EXPECTED_NPM_PACK_UNPACKED'"
    else
        fail "npm pack output missing '$EXPECTED_NPM_PACK_UNPACKED' (see $logf)"
    fi

    if grep -E "aegisgate-sdk-0\.1\.0\.tgz|@aegisgate/sdk@0\.1\.0" "$logf" >/dev/null 2>&1; then
        pass "npm pack reports aegisgate sdk @ 0.1.0"
    else
        fail "npm pack did not report version 0.1.0"
    fi
}

test_full_install() {
    # --full 模式：真实 pip install local wheel + import smoke
    # CI 不跑这个；user 本地 verify 用。
    group "test_full_install (--full only)"

    if ! python3 -c "import build" 2>/dev/null; then
        info "skip — python 'build' not installed"
        return
    fi

    local distdir venvdir
    distdir=$(mktemp -d)
    venvdir=$(mktemp -d)

    if ! (cd "$PY_DIR" && python3 -m build --outdir "$distdir" >/dev/null 2>&1); then
        fail "python -m build failed"
        rm -rf "$distdir" "$venvdir"
        return
    fi

    if ! python3 -m venv "$venvdir/venv" >/dev/null 2>&1; then
        info "skip — python venv unavailable"
        rm -rf "$distdir" "$venvdir"
        return
    fi

    local wheel
    wheel=$(ls "$distdir"/aegisgate-0.1.0-*.whl 2>/dev/null | head -1)
    if [[ -z "$wheel" ]]; then
        fail "no wheel found in $distdir"
        rm -rf "$distdir" "$venvdir"
        return
    fi

    local pip="$venvdir/venv/bin/pip"
    local python="$venvdir/venv/bin/python"

    if "$pip" install "$wheel" >/tmp/sdk-full-install.log 2>&1; then
        if grep -F "$EXPECTED_INSTALL_OK" /tmp/sdk-full-install.log >/dev/null 2>&1; then
            pass "pip install reports '$EXPECTED_INSTALL_OK'"
        else
            fail "pip install missing '$EXPECTED_INSTALL_OK'"
        fi
    else
        fail "pip install failed (see /tmp/sdk-full-install.log)"
    fi

    local imported_version
    imported_version=$("$python" -c 'import aegisgate; print(aegisgate.__version__)' 2>/dev/null || echo "ERROR")
    if [[ "$imported_version" == "$EXPECTED_IMPORT_VERSION" ]]; then
        pass "import aegisgate; __version__ == '$EXPECTED_IMPORT_VERSION'"
    else
        fail "import aegisgate; __version__ == '$imported_version' (expected '$EXPECTED_IMPORT_VERSION')"
    fi

    rm -rf "$distdir" "$venvdir"
}

# ---------- Runner ----------

echo "[test-sdk-publish] mode=${MODE} root=${ROOT_DIR}"

test_python_metadata
test_nodejs_metadata
test_sr1_no_token_literals
test_sr3_tag_only_trigger
test_sr4_keyword_hardcode
test_python_build_dry
test_nodejs_pack_dry

if [[ "$MODE" == "full" ]]; then
    test_full_install
fi

echo ""
echo "=== Summary ==="
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"

if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
exit 0
