#!/usr/bin/env bash
# 防漂移审计测试：TASK-20260618-01 生产环境代码 review 与验证
#
# 用法：bash tests/audit/test-task20260618-01-prod-review.sh
#
# 断言分组（按 spec §4 六维度 + §5 P1 修复正确性 + §6 验证清单）：
#   D5-① 依赖供应链（A1.1-A1.5）— overlay-port / vcpkg.json features 一致 / abseil 补链
#   D5-② 默认口令权限（A2.1-A2.3）— 默认 PG 口令不可固定 / creds 文件 chmod 600
#   D5-③ 网络暴露面（A3.1）  — setup-prod-deps OTel 127.0.0.1 监听
#   D5-④ Secret 默认值（A4.1-A4.3）— jwt_secret REQUIRED / OTel deb SHA256 验证
#   D5-⑤ 文件权限（A5.1）— Dockerfile 非 root USER
#   D5-⑥ 日志卫生（A6.1）— Dockerfile prod 档位 ARG / docker-compose build.args / CHANGELOG
#
# 输出：每断言 [PASS]/[FAIL] 行 + 末尾汇总；任意 FAIL 整体退出 1。
#
# 初态预期：P1×4 相关断言 FAIL（A2.1 / A2.3 / A4.3 / A6.1.1-A6.1.6 / A1.5b? / 等共 ~12 项）；
# E3-E5 修复后期望全 GREEN。

set -uo pipefail   # 不开 -e（让单点断言失败不中断脚本，全跑完再汇总）

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

PASS=0
FAIL=0
FAILED_ASSERTIONS=()

assert_contains() {
  local label="$1"; local pattern="$2"; local file="$3"
  if [ ! -f "$file" ]; then
    echo "[FAIL] ${label} — 文件不存在: ${file}"
    FAIL=$((FAIL + 1)); FAILED_ASSERTIONS+=("${label}")
    return
  fi
  if grep -qE "$pattern" "$file"; then
    echo "[PASS] ${label}"
    PASS=$((PASS + 1))
  else
    echo "[FAIL] ${label} — pattern not found: ${pattern} in ${file}"
    FAIL=$((FAIL + 1)); FAILED_ASSERTIONS+=("${label}")
  fi
}

assert_not_contains() {
  local label="$1"; local pattern="$2"; local file="$3"
  if [ ! -f "$file" ]; then
    echo "[FAIL] ${label} — 文件不存在: ${file}"
    FAIL=$((FAIL + 1)); FAILED_ASSERTIONS+=("${label}")
    return
  fi
  if grep -qE "$pattern" "$file"; then
    echo "[FAIL] ${label} — pattern should NOT match: ${pattern} in ${file}"
    FAIL=$((FAIL + 1)); FAILED_ASSERTIONS+=("${label}")
  else
    echo "[PASS] ${label}"
    PASS=$((PASS + 1))
  fi
}

assert_bash_n() {
  local label="$1"; local file="$2"
  if [ ! -f "$file" ]; then
    echo "[FAIL] ${label} — 文件不存在: ${file}"
    FAIL=$((FAIL + 1)); FAILED_ASSERTIONS+=("${label}")
    return
  fi
  if bash -n "$file" 2>/dev/null; then
    echo "[PASS] ${label}"
    PASS=$((PASS + 1))
  else
    echo "[FAIL] ${label} — bash -n syntax error: ${file}"
    FAIL=$((FAIL + 1)); FAILED_ASSERTIONS+=("${label}")
  fi
}

assert_file_absent() {
  local label="$1"; local file="$2"
  if [ -e "$file" ]; then
    echo "[FAIL] ${label} — obsolete file still exists: ${file}"
    FAIL=$((FAIL + 1)); FAILED_ASSERTIONS+=("${label}")
  else
    echo "[PASS] ${label}"
    PASS=$((PASS + 1))
  fi
}

echo "==============================================================="
echo "TASK-20260618-01 audit drift guard test (spec §4 + §5)"
echo "==============================================================="
echo ""

# ─────────────────────────────────────────────────────────────────
# 维度 ①：依赖供应链
# ─────────────────────────────────────────────────────────────────
echo "── 维度 ① 依赖供应链 ──"
assert_contains "A1.1 overlay-port 上游 = google/sentencepiece" \
  '^[[:space:]]+REPO google/sentencepiece' \
  vcpkg-overlay-ports/sentencepiece/portfile.cmake
assert_contains "A1.2 overlay-port REF = v0.2.1" \
  '^[[:space:]]+REF "v\$\{VERSION\}"' \
  vcpkg-overlay-ports/sentencepiece/portfile.cmake
assert_contains "A1.3 overlay-port SPM_PROTOBUF_PROVIDER=package" \
  '\-DSPM_PROTOBUF_PROVIDER=package' \
  vcpkg-overlay-ports/sentencepiece/portfile.cmake
assert_contains "A1.4 vcpkg-configuration.json overlay-ports 已声明" \
  '"overlay-ports"' \
  vcpkg-configuration.json
assert_contains "A1.5 src/CMakeLists.txt 补链 absl::flags_parse" \
  'absl::flags_parse' \
  src/CMakeLists.txt
assert_contains "A1.5b src/CMakeLists.txt 补链 absl::strings" \
  'absl::strings' \
  src/CMakeLists.txt
# 生产档位默认 AEGIS_VCPKG_FEATURES 与 vcpkg.json features 集合一致
# （生产构建逻辑已整合进统一的 scripts/build.sh，由构建类型驱动档位）
assert_contains "A1.6a0 build.sh 生产档位 AEGIS_VCPKG_FEATURES 含 guard" \
  'AEGIS_VCPKG_FEATURES.*guard;' \
  scripts/build.sh
assert_contains "A1.6a build.sh 生产档位 AEGIS_VCPKG_FEATURES 含 guard-spm" \
  'AEGIS_VCPKG_FEATURES.*guard-spm' \
  scripts/build.sh
assert_contains "A1.6b build.sh 生产档位 AEGIS_VCPKG_FEATURES 含 redis" \
  'AEGIS_VCPKG_FEATURES.*redis' \
  scripts/build.sh
assert_contains "A1.6c build.sh 生产档位 AEGIS_VCPKG_FEATURES 含 pg" \
  'AEGIS_VCPKG_FEATURES.*pg' \
  scripts/build.sh
assert_contains "A1.6d build.sh 生产档位 AEGIS_VCPKG_FEATURES 含 otel" \
  'AEGIS_VCPKG_FEATURES.*otel' \
  scripts/build.sh
assert_contains "A1.6e build.sh 生产档位 AEGIS_VCPKG_FEATURES 含 control-plane" \
  'AEGIS_VCPKG_FEATURES.*control-plane' \
  scripts/build.sh
assert_file_absent "A1.6f obsolete scripts/build-prod.sh 不应作为独立入口存在" \
  scripts/build-prod.sh
assert_contains "A1.6g production runbook 列出 libpq 构建依赖 apt 命令" \
  'autoconf automake libtool autoconf-archive bison flex' \
  docs/guides/production-validation-runbook.md
assert_contains "A1.6h 中文 production runbook 列出 libpq 构建依赖 apt 命令" \
  'autoconf automake libtool autoconf-archive bison flex' \
  docs/guides/production-validation-runbook_zh.md

# ─────────────────────────────────────────────────────────────────
# 维度 ②：默认口令与权限最小化（P1-A 相关，初态部分 FAIL）
# ─────────────────────────────────────────────────────────────────
echo ""
echo "── 维度 ② 默认口令权限 ──"
# A2.1 P1-A 修复后断言：默认 PG 口令不可固定为字面量 "aegisgate"（即不可在 sh 顶部
# 出现 `PG_PASSWORD="aegisgate"` 形式的默认赋值）
assert_not_contains "A2.1 setup-prod-deps.sh 默认 PG 口令不为字面量 aegisgate（P1-A）" \
  '^PG_PASSWORD="aegisgate"$' \
  scripts/setup-prod-deps.sh
# A2.2 P1-A 修复正确性：脚本里调用 openssl rand -hex 24（或同等强度）生成口令
assert_contains "A2.2 setup-prod-deps.sh 使用 openssl rand -hex 24 生成 PG 口令（P1-A）" \
  'openssl rand -hex 24' \
  scripts/setup-prod-deps.sh
# A2.3 P1-A 修复正确性：写入 ~/.aegisgate_prod_creds（chmod 600）
assert_contains "A2.3 setup-prod-deps.sh 凭据文件 chmod 600（P1-A）" \
  'chmod 600' \
  scripts/setup-prod-deps.sh
assert_contains "A2.3b setup-prod-deps.sh 凭据文件命名 .aegisgate_prod_creds（P1-A）" \
  '\.aegisgate_prod_creds' \
  scripts/setup-prod-deps.sh

# ─────────────────────────────────────────────────────────────────
# 维度 ③：网络暴露面
# ─────────────────────────────────────────────────────────────────
echo ""
echo "── 维度 ③ 网络暴露面 ──"
assert_contains "A3.1 setup-prod-deps.sh OTel gRPC 监听 127.0.0.1:4317" \
  'endpoint: 127\.0\.0\.1:4317' \
  scripts/setup-prod-deps.sh
assert_contains "A3.2 setup-prod-deps.sh OTel HTTP 监听 127.0.0.1:4318" \
  'endpoint: 127\.0\.0\.1:4318' \
  scripts/setup-prod-deps.sh

# ─────────────────────────────────────────────────────────────────
# 维度 ④：Secret 默认值（P1-B 相关，初态 FAIL）
# ─────────────────────────────────────────────────────────────────
echo ""
echo "── 维度 ④ Secret 默认值 ──"
assert_contains "A4.1 config/aegisgate.yaml jwt_secret REQUIRED env" \
  'jwt_secret.*REQUIRED.*AEGISGATE_ADMIN_JWT_SECRET' \
  config/aegisgate.yaml
assert_contains "A4.2 package.sh start.sh 启动预检 AEGISGATE_API_KEY 阻断" \
  'AEGISGATE_API_KEY' \
  scripts/package.sh
# A4.3 P1-B 修复后断言：OTel deb 下载校验 SHA256
assert_contains "A4.3 setup-prod-deps.sh OTel deb 加 sha256sum -c 校验（P1-B）" \
  'sha256sum -c' \
  scripts/setup-prod-deps.sh
assert_contains "A4.3b setup-prod-deps.sh OTel SHA256 map 定义（P1-B）" \
  'OTEL_SHA256' \
  scripts/setup-prod-deps.sh

# ─────────────────────────────────────────────────────────────────
# 维度 ⑤：文件权限与所有权
# ─────────────────────────────────────────────────────────────────
echo ""
echo "── 维度 ⑤ 文件权限 ──"
assert_contains "A5.1 Dockerfile 运行时 USER aegisgate" \
  '^USER aegisgate$' \
  Dockerfile

# ─────────────────────────────────────────────────────────────────
# 维度 ⑥：日志卫生 + Dockerfile 生产档位（P1-C / P1-D 相关，初态 FAIL）
# ─────────────────────────────────────────────────────────────────
echo ""
echo "── 维度 ⑥ 日志卫生 + 生产档位对齐（P1-C / P1-D） ──"
# A6.1.1 P1-C 修复后断言：Dockerfile 新增 ARG ENABLE_PG
assert_contains "A6.1.1 Dockerfile ARG ENABLE_PG（P1-C）" \
  '^ARG ENABLE_PG=' \
  Dockerfile
# A6.1.2 P1-C 修复后断言：Dockerfile 新增 ARG ENABLE_CONTROL_PLANE
assert_contains "A6.1.2 Dockerfile ARG ENABLE_CONTROL_PLANE（P1-C）" \
  '^ARG ENABLE_CONTROL_PLANE=' \
  Dockerfile
# A6.1.3 P1-C 修复后断言：Dockerfile cmake 步骤透传 ENABLE_PG
assert_contains "A6.1.3 Dockerfile cmake 透传 -DENABLE_PG（P1-C）" \
  '\-DENABLE_PG=\$\{ENABLE_PG\}' \
  Dockerfile
# A6.1.4 P1-C 修复后断言：Dockerfile cmake 步骤透传 ENABLE_CONTROL_PLANE
assert_contains "A6.1.4 Dockerfile cmake 透传 -DENABLE_CONTROL_PLANE（P1-C）" \
  '\-DENABLE_CONTROL_PLANE=\$\{ENABLE_CONTROL_PLANE\}' \
  Dockerfile
# A6.1.5 P1-C 修复后断言：docker-compose aegisgate.build.args 含 ENABLE_PG
assert_contains "A6.1.5 docker-compose.yaml build.args 含 ENABLE_PG（P1-C）" \
  'ENABLE_PG:[[:space:]]*\$\{ENABLE_PG' \
  docker-compose.yaml
# A6.1.6 P1-C 修复后断言：docker-compose aegisgate.build.args 含 ENABLE_CONTROL_PLANE
assert_contains "A6.1.6 docker-compose.yaml build.args 含 ENABLE_CONTROL_PLANE（P1-C）" \
  'ENABLE_CONTROL_PLANE:[[:space:]]*\$\{ENABLE_CONTROL_PLANE' \
  docker-compose.yaml
# A6.1.7 P1-C 修复后断言：production-deployment 双语提到容器化生产档位
assert_contains "A6.1.7 production-deployment.md 提及容器化生产档位（P1-C）" \
  'ENABLE_PG.*ON|build-prod\.sh|生产档位|prod profile' \
  docs/guides/production-deployment.md
assert_contains "A6.1.8 production-deployment_zh.md 提及容器化生产档位（P1-C / C8 对称）" \
  'ENABLE_PG.*ON|build-prod\.sh|生产档位|prod profile' \
  docs/guides/production-deployment_zh.md

# A6.2 P1-D 修复后断言：CHANGELOG [Unreleased] 提到 3542b0c / prod profile / SIGSEGV / overlay-port
assert_contains "A6.2.1 CHANGELOG [Unreleased] 含 3542b0c 或生产档位 SIGSEGV 修复（P1-D）" \
  '3542b0c|生产档位.*SIGSEGV|生产档位.*段错误|overlay-port.*sentencepiece' \
  CHANGELOG.md
assert_contains "A6.2.2 CHANGELOG [Unreleased] 含统一生产构建入口（P1-D）" \
  'scripts/build\.sh -t Release' \
  CHANGELOG.md
assert_contains "A6.2.3 CHANGELOG [Unreleased] 含 setup-prod-deps.sh（P1-D）" \
  'setup-prod-deps\.sh' \
  CHANGELOG.md
assert_contains "A6.2.4 CHANGELOG [Unreleased] 含 TASK-20260618-01（P1-D）" \
  'TASK-20260618-01' \
  CHANGELOG.md

# ─────────────────────────────────────────────────────────────────
# 静态健康检查（spec §6.1 V1-V2）
# ─────────────────────────────────────────────────────────────────
echo ""
echo "── 静态健康检查 ──"
assert_bash_n "V1.1 bash -n scripts/build.sh" scripts/build.sh
assert_bash_n "V1.2 bash -n scripts/setup-prod-deps.sh" scripts/setup-prod-deps.sh
assert_bash_n "V1.3 bash -n scripts/package.sh" scripts/package.sh
assert_contains "V2.1 scripts/build.sh has 'set -euo pipefail'" \
  '^set -euo pipefail$' \
  scripts/build.sh
assert_contains "V2.2 scripts/setup-prod-deps.sh has 'set -euo pipefail'" \
  '^set -euo pipefail$' \
  scripts/setup-prod-deps.sh
assert_not_contains "V2.3 live docs/code 不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh|rebuild with scripts/build-prod\.sh|build-prod cmake' \
  docs/guides/production-validation-runbook.md
assert_not_contains "V2.4 中文 runbook 不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh|rebuild with scripts/build-prod\.sh|build-prod cmake' \
  docs/guides/production-validation-runbook_zh.md
assert_not_contains "V2.5 acceptance 文档不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh|build-prod cmake|查看 `build-prod`' \
  docs/acceptance/acceptance-test-plan.md
assert_not_contains "V2.6 result template 不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh' \
  docs/acceptance/prod-validation-result-template.md
assert_not_contains "V2.7 quick-start 文档不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh' \
  docs/guides/quick-start.md
assert_not_contains "V2.8 quick-start 中文文档不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh' \
  docs/guides/quick-start_zh.md
assert_not_contains "V2.9 production-deployment 文档不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh' \
  docs/guides/production-deployment.md
assert_not_contains "V2.10 production-deployment 中文文档不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh' \
  docs/guides/production-deployment_zh.md
assert_not_contains "V2.11 runtime 错误提示不再引用已移除 scripts/build-prod.sh" \
  'scripts/build-prod\.sh|rebuild with scripts/build-prod\.sh' \
  src/core/pipeline_assembler.cpp

# ─────────────────────────────────────────────────────────────────
# 汇总
# ─────────────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL))
echo ""
echo "==============================================================="
echo "Result: ${PASS}/${TOTAL} PASS, ${FAIL} FAIL"
if [ "$FAIL" -gt 0 ]; then
  echo ""
  echo "Failed assertions:"
  for a in "${FAILED_ASSERTIONS[@]}"; do
    echo "  - ${a}"
  done
fi
echo "==============================================================="
[ "$FAIL" -eq 0 ]
