#!/usr/bin/env bash
set -euo pipefail

# AegisGate 生产依赖一键安装脚本 (Ubuntu / Debian, apt + systemd)
#
# 为「在本机原生运行生产形态」准备 build.sh 生产档位(-t Release)产物所需的后端服务：
#   - Redis        共享缓存          (监听 127.0.0.1:6379)
#   - PostgreSQL   持久化            (库/用户/口令 = aegisgate, 127.0.0.1:5432)
#   - OTel Collector  分布式追踪 sink (OTLP HTTP 127.0.0.1:4318，可选)
#
# 库名/口令与仓库 docker-compose.yaml 保持一致，便于两套环境互换。
# 幂等：可重复执行；已存在的库/用户/服务不会重复创建。
#
# 用法:  sudo ./scripts/setup-prod-deps.sh [选项]
#   --no-otel              跳过 OTel Collector(只装 redis + postgres)
#   --otel-version VER     指定 otelcol-contrib 版本(默认 0.110.0)
#   --pg-password PASS     PostgreSQL aegisgate 用户口令(未传则随机生成 24
#                          hex 字节并写入 ~/.aegisgate_prod_creds (chmod 600))
#   -h, --help             显示帮助
#
# 注意：本脚本会修改系统(apt 安装、创建 systemd 服务、建 PG 库)，需 root。

OTEL_VERSION="0.110.0"
# P1-A（TASK-20260618-01 audit）：默认不再使用固定弱口令 "aegisgate"。
# 未传 --pg-password 时由下方逻辑生成 `openssl rand -hex 24`（192 bit 熵）
# 一次性随机口令，并写入 ~/.aegisgate_prod_creds（chmod 600 / SR4）。
PG_PASSWORD=""
PG_PASSWORD_GENERATED=false
INSTALL_OTEL=true

# P1-B（TASK-20260618-01 audit）：OTel Collector deb 下载完整性校验。
# 升级 OTEL_VERSION 时**必须**同步更新此表（用 `sha256sum` 计算下载产物）。
# 未维护版本/架构的 SHA256 → verify_otel_deb 拒绝继续（fail-closed）。
declare -A OTEL_SHA256_MAP=(
  ["0.110.0_amd64"]="0a32f9bfcf4db39178efbe6b62bded14e8da8744cfc641718e852bf7989f6f09"
  ["0.110.0_arm64"]="f920e7eeebf70ee564ac897d72ddbd32fbc95bf930a6a14a8c2687274aabd3d4"
)

usage() { sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --no-otel)        INSTALL_OTEL=false; shift ;;
    --otel-version)   OTEL_VERSION="$2"; shift 2 ;;
    --pg-password)    PG_PASSWORD="$2"; shift 2 ;;
    -h|--help)        usage; exit 0 ;;
    *) echo "未知参数: $1" >&2; usage; exit 1 ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "错误: 需要 root。请用: sudo $0 $*" >&2
  exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "错误: 本脚本仅支持 apt 系发行版(Ubuntu/Debian)。" >&2
  exit 1
fi

ARCH_DEB=$(dpkg --print-architecture)   # amd64 / arm64
log() { echo "[setup-prod-deps] $*"; }

# P1-A（TASK-20260618-01 audit）：随机生成 24 hex 字节(192 bit 熵)默认 PG 口令。
# `openssl rand -hex 24` 比 distro 默认的 /dev/urandom + base64 更可移植。
# 仅在用户未显式 --pg-password 时生成；生成后会写入 ~/.aegisgate_prod_creds（chmod 600）。
if [ -z "$PG_PASSWORD" ]; then
  if ! command -v openssl >/dev/null 2>&1; then
    echo "错误: 需要 openssl 生成随机 PG 口令（未传 --pg-password 时必需）。" >&2
    echo "      apt-get install -y openssl 或显式 --pg-password <PASSWORD>" >&2
    exit 1
  fi
  PG_PASSWORD="$(openssl rand -hex 24)"
  PG_PASSWORD_GENERATED=true
fi

# P1-B（TASK-20260618-01 audit）：OTel Collector deb 下载完整性校验。
# fail-closed：未维护 SHA256 / hash 不匹配 → 删除产物 + 退出（不安装）。
# 升级 OTEL_VERSION 时务必同步 OTEL_SHA256_MAP（顶部）。
verify_otel_deb() {
  local file="$1"; local expected="$2"
  if [ -z "$expected" ]; then
    echo "错误: 未维护 OTel ${OTEL_VERSION}_${ARCH_DEB} 的 SHA256；" >&2
    echo "      请先在 setup-prod-deps.sh OTEL_SHA256_MAP 补全后重试。" >&2
    rm -f "$file"
    exit 1
  fi
  if ! echo "${expected}  ${file}" | sha256sum -c - >/dev/null 2>&1; then
    local actual
    actual=$(sha256sum "$file" 2>/dev/null | awk '{print $1}')
    echo "错误: OTel deb SHA256 校验失败" >&2
    echo "      expected: ${expected}" >&2
    echo "      actual:   ${actual}" >&2
    rm -f "$file"
    exit 1
  fi
}

# ---------------------------------------------------------------------------
log "1/4 apt 更新并安装 Redis + PostgreSQL ..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y redis-server postgresql postgresql-contrib curl ca-certificates

# ---------------------------------------------------------------------------
log "2/4 启用并启动 Redis (127.0.0.1:6379, 无口令) ..."
systemctl enable --now redis-server
systemctl is-active --quiet redis-server && log "  ✓ redis-server 运行中" || log "  ⚠ redis-server 未运行，请检查"

# ---------------------------------------------------------------------------
log "3/4 配置 PostgreSQL 库/用户 (aegisgate) ..."
systemctl enable --now postgresql

# 幂等创建用户与库；用户口令每次执行都对齐到 --pg-password。
sudo -u postgres psql -v ON_ERROR_STOP=1 <<SQL
DO \$\$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'aegisgate') THEN
    CREATE ROLE aegisgate LOGIN PASSWORD '${PG_PASSWORD}';
  ELSE
    ALTER ROLE aegisgate WITH PASSWORD '${PG_PASSWORD}';
  END IF;
END
\$\$;
SELECT 'CREATE DATABASE aegisgate OWNER aegisgate'
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'aegisgate')\gexec
GRANT ALL PRIVILEGES ON DATABASE aegisgate TO aegisgate;
SQL
log "  ✓ PostgreSQL: 库=aegisgate 用户=aegisgate 已就绪"

# ---------------------------------------------------------------------------
if [ "$INSTALL_OTEL" = true ]; then
  log "4/4 安装 OpenTelemetry Collector v${OTEL_VERSION} (OTLP HTTP 127.0.0.1:4318) ..."
  if command -v otelcol-contrib >/dev/null 2>&1; then
    log "  ✓ otelcol-contrib 已安装，跳过下载"
  else
    DEB="otelcol-contrib_${OTEL_VERSION}_linux_${ARCH_DEB}.deb"
    URL="https://github.com/open-telemetry/opentelemetry-collector-releases/releases/download/v${OTEL_VERSION}/${DEB}"
    TMP="$(mktemp -d)"
    log "  下载 $URL"
    curl -fSL "$URL" -o "$TMP/$DEB"
    # P1-B（TASK-20260618-01 audit）：下载后立即校验 SHA256；失败即清理退出。
    EXPECTED_SHA256="${OTEL_SHA256_MAP[${OTEL_VERSION}_${ARCH_DEB}]:-}"
    log "  校验 SHA256 (${OTEL_VERSION}_${ARCH_DEB})"
    verify_otel_deb "$TMP/$DEB" "$EXPECTED_SHA256"
    apt-get install -y "$TMP/$DEB"
    rm -rf "$TMP"
  fi

  # 最小化配置：接收 OTLP(gRPC 4317 / HTTP 4318)，导出到日志(本地验证用的 sink)。
  # 生产可改为转发到 Tempo/Jaeger 等真实后端。
  install -d /etc/otelcol-contrib
  cat > /etc/otelcol-contrib/config.yaml <<'YAML'
receivers:
  otlp:
    protocols:
      grpc:
        endpoint: 127.0.0.1:4317
      http:
        endpoint: 127.0.0.1:4318
processors:
  batch: {}
exporters:
  debug:
    verbosity: normal
service:
  pipelines:
    traces:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug]
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug]
YAML
  systemctl enable --now otelcol-contrib
  systemctl restart otelcol-contrib
  systemctl is-active --quiet otelcol-contrib && log "  ✓ otelcol-contrib 运行中" || log "  ⚠ otelcol-contrib 未运行，journalctl -u otelcol-contrib 查看"
else
  log "4/4 跳过 OTel Collector (--no-otel)"
fi

# ---------------------------------------------------------------------------
# P1-A + SR4（TASK-20260618-01 audit）：随机生成口令必须**仅**写入 chmod 600 文件，
# 不在 stdout/journalctl 持久化明文口令。$SUDO_USER 存在时归属调用者；否则写 /root。
CREDS_HOME="${SUDO_USER:+/home/$SUDO_USER}"
CREDS_HOME="${CREDS_HOME:-/root}"
CREDS_FILE="${CREDS_HOME}/.aegisgate_prod_creds"

if [ "$PG_PASSWORD_GENERATED" = true ]; then
  umask 077
  cat > "$CREDS_FILE" <<CREDS
# AegisGate 生产依赖凭据（由 scripts/setup-prod-deps.sh 自动生成 / chmod 600）
# 生成时间: $(date -u +%Y-%m-%dT%H:%M:%SZ)
# OTel 版本: ${OTEL_VERSION}
# 注意: 必须用 export，这样 `source` 本文件后子进程(aegisgate)才能经
#       getenv("POSTGRES_URL") 读到；无 export 仅是 shell 局部变量，
#       网关启动时 storage.postgres.url=\${POSTGRES_URL} 会解析为空 →
#       libpq 回退默认 unix socket → PG 连接失败 → strict 模式拒绝启动。
export POSTGRES_URL=postgres://aegisgate:${PG_PASSWORD}@127.0.0.1:5432/aegisgate
export PG_PASSWORD=${PG_PASSWORD}
CREDS
  chmod 600 "$CREDS_FILE"
  if [ -n "${SUDO_USER:-}" ]; then
    chown "$SUDO_USER:$SUDO_USER" "$CREDS_FILE" 2>/dev/null || true
  fi
  echo "[setup-prod-deps] 已生成随机 PG 口令并写入 $CREDS_FILE (chmod 600)" >&2
  echo "[setup-prod-deps] 请妥善保存：丢失后需 --pg-password <NEW> 重跑覆盖。" >&2
  PG_PASSWORD_HINT="(随机生成 / 见 ${CREDS_FILE})"
else
  PG_PASSWORD_HINT="(由 --pg-password 显式提供 / 凭据未写入文件)"
fi

# 应用层密钥模板（与 PG 凭据分离 / 方案 B）：网关 Key、管理台 JWT、模型供应商 Key
# 由**你**维护，不由本脚本生成。仅在文件不存在时写出占位模板，已存在则原样保留——
# 因此重跑本脚本（含随机轮换 PG 口令）永远不会覆盖你填好的密钥。start.sh 启动时
# 会自动 source 本文件。务必保持 chmod 600（同机他用户无法读取明文）。
APP_SECRETS_FILE="${CREDS_HOME}/.aegisgate_app_secrets"
if [ ! -f "$APP_SECRETS_FILE" ]; then
  umask 077
  cat > "$APP_SECRETS_FILE" <<'APPSECRETS'
# AegisGate 应用层密钥（你自行维护；scripts/setup-prod-deps.sh 不会覆盖本文件）
# 填好后 start.sh 会自动 source。务必保持 chmod 600。
# 注意：这里是你的私有密钥（sk-... 等），切勿提交到版本库或打印到日志。

# 网关访问 Key（客户端 Authorization: Bearer 使用）
export AEGISGATE_API_KEY=""

# Web 管理台 JWT 签名密钥（建议: openssl rand -hex 32）
export AEGISGATE_ADMIN_JWT_SECRET=""

# 模型供应商 Key（按需填一个或多个；未用到的保持注释）
export DEEPSEEK_API_KEY=""
#export OPENAI_API_KEY=""
#export CLAUDE_API_KEY=""
#export QWEN_API_KEY=""
#export GEMINI_API_KEY=""
#export DOUBAO_API_KEY=""
#export MISTRAL_API_KEY=""
APPSECRETS
  chmod 600 "$APP_SECRETS_FILE"
  if [ -n "${SUDO_USER:-}" ]; then
    chown "$SUDO_USER:$SUDO_USER" "$APP_SECRETS_FILE" 2>/dev/null || true
  fi
  echo "[setup-prod-deps] 已生成应用密钥模板 $APP_SECRETS_FILE (chmod 600)，请填入真实 Key。" >&2
else
  echo "[setup-prod-deps] 应用密钥文件 $APP_SECRETS_FILE 已存在，保留不动。" >&2
fi

cat <<DONE

========================================================
 生产依赖安装完成
========================================================
 Redis:       127.0.0.1:6379  (无口令)
 PostgreSQL:  127.0.0.1:5432  库/用户=aegisgate 口令=${PG_PASSWORD_HINT}
$([ "$INSTALL_OTEL" = true ] && echo " OTLP:        http://127.0.0.1:4318 (otelcol-contrib, 导出到 debug 日志)")

 下一步(让网关真正用上它们):
   1. 在 config/aegisgate.yaml 把后端切换为:
        cache_backend: redis
        persistent_backend: postgres
      并填好 redis/postgres 连接信息(参考文件内 55/63 行注释)。
   2. 密钥分两个 chmod 600 文件（start.sh 启动时会自动 source，无需手动 export）:
        # PG 连接串（本脚本自动生成 / 可被随机重跑覆盖）
        ${CREDS_FILE}
        # 应用层密钥（你自行编辑填入；重跑本脚本不会覆盖）
        ${APP_SECRETS_FILE:-${CREDS_HOME}/.aegisgate_app_secrets}
      编辑后者填入真实 Key:
        AEGISGATE_API_KEY / AEGISGATE_ADMIN_JWT_SECRET / 模型供应商 Key
        管理台 JWT 可用: openssl rand -hex 32
   3. (可选)下载 guard 模型: ./scripts/download_guard_model.sh models/guard
   4. 启动: 解压分发 tarball 后 ./start.sh --profile prod
      (start.sh 会自动从 \$HOME 读取上面两个密钥文件)
========================================================
DONE
