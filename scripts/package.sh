#!/usr/bin/env bash
set -euo pipefail

# AegisGate 打包分发脚本
# 用法: ./scripts/package.sh
#
# 前置: 先运行 ./scripts/build.sh 完成编译
# 产出: dist/aegisgate-<version>-<os>-<arch>.tar.gz

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

# --- Preflight check ---
if [ ! -f build/src/aegisgate ]; then
  echo "错误: build/src/aegisgate 不存在"
  echo "请先运行: ./scripts/build.sh"
  exit 1
fi

VERSION=$(grep -oP 'project\(aegisgate VERSION \K[0-9.]+' CMakeLists.txt 2>/dev/null | head -1 || true)
# PKG_VERSION / PKG_ARCH 允许调用方（如 scripts/build.sh -v/-p）覆盖分发包
# 命名标签，便于按发布版本/目标架构打包；未设置时回退到 CMakeLists 版本与本机架构。
VERSION="${PKG_VERSION:-${VERSION:-0.0.0}}"
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH="${PKG_ARCH:-$(uname -m)}"
PACKAGE_NAME="aegisgate-${VERSION}-${OS}-${ARCH}"
DIST_DIR="dist/${PACKAGE_NAME}"

echo "========================================"
echo " AegisGate Packager"
echo " Version: ${VERSION}"
echo " Target:  ${OS}-${ARCH}"
echo "========================================"
echo ""

# --- Assemble ---
echo "[1/2] 组装分发包..."
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"/{config/rules,web/admin}

# Binary
cp build/src/aegisgate "$DIST_DIR/"
[ -f build/aegisctl ] && cp build/aegisctl "$DIST_DIR/"

# Config 模板（.example）：真实 config 由 start.sh 首启 seeding 生成，
# 因此原地解压升级时真实 config 永不在包内、永不被覆盖。
cp config/aegisgate.yaml "$DIST_DIR/config/aegisgate.yaml.example"
cp config/models.yaml "$DIST_DIR/config/models.yaml.example"
# TASK-20260622-01 E3 (G2): 携带生产档位模板，使分发包能以 prod 档位运行
# （start.sh --profile prod / AEGISGATE_PROFILE=prod 选用），而非默认掉回 community。
if [ -f config/aegisgate.prod.yaml ]; then
  cp config/aegisgate.prod.yaml "$DIST_DIR/config/aegisgate.prod.yaml.example"
  echo "  ✓ 生产档位模板已包含 (config/aegisgate.prod.yaml.example)"
fi
if [ -d config/rules ]; then
  for f in config/rules/*.yaml; do
    [ -e "$f" ] && cp "$f" "$DIST_DIR/config/rules/$(basename "$f").example"
  done
fi

# Frontend
if [ -d web/admin/dist ]; then
  cp -r web/admin/dist "$DIST_DIR/web/admin/"
  echo "  ✓ Web 管理面板已包含"
else
  echo "  ⚠ web/admin/dist 不存在，跳过（可运行 ./scripts/build.sh --frontend 构建）"
fi

# ONNX Runtime 动态库（SR-2：分发包不依赖开发机绝对 RUNPATH）
# 解析 aegisgate 实际链接的 libonnxruntime 路径，拷到 dist/<pkg>/lib/，
# 由 start.sh 经 LD_LIBRARY_PATH 加载，避免目标机缺库或命中开发机绝对路径。
mkdir -p "$DIST_DIR/lib"
if [ "$OS" = "darwin" ]; then
  ORT_LIB=$(otool -L build/src/aegisgate 2>/dev/null | awk '/libonnxruntime/{print $1}' | head -1 || true)
else
  ORT_LIB=$(ldd build/src/aegisgate 2>/dev/null | awk '/libonnxruntime/{print $3}' | head -1 || true)
fi
if [ -n "${ORT_LIB:-}" ] && [ -e "$ORT_LIB" ]; then
  ORT_LIBDIR=$(dirname "$ORT_LIB")
  # -P 保留 symlink 链（libonnxruntime.so -> .so.1 -> .so.1.24.2）
  cp -P "$ORT_LIBDIR"/libonnxruntime.so* "$DIST_DIR/lib/" 2>/dev/null || true
  cp -P "$ORT_LIBDIR"/libonnxruntime.*dylib "$DIST_DIR/lib/" 2>/dev/null || true
  cp "$ORT_LIBDIR"/libonnxruntime_providers_shared.* "$DIST_DIR/lib/" 2>/dev/null || true
  LIB_COUNT=$(find "$DIST_DIR/lib" -maxdepth 1 -name 'libonnxruntime*' | wc -l | tr -d ' ')
  echo "  ✓ ONNX Runtime 动态库已包含 (${LIB_COUNT} 个 -> lib/)"
else
  echo "  ⚠ 未检测到 libonnxruntime 链接（aegisgate 可能未编入 ONNX）；lib/ 留空"
fi

# 模型下载脚本随包（D2：首启提示下载，不在 tar 内塞 832MB 模型）
mkdir -p "$DIST_DIR/scripts"
cp scripts/download_model.sh scripts/download_guard_model.sh "$DIST_DIR/scripts/"
chmod +x "$DIST_DIR/scripts/download_model.sh" "$DIST_DIR/scripts/download_guard_model.sh"
echo "  ✓ 模型下载脚本已包含 (scripts/download_model.sh, download_guard_model.sh)"

# Startup script
cat > "$DIST_DIR/start.sh" << 'STARTUP'
#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APP_DIR"

# --- 0. 动态库路径（SR-2：让随包的 ONNX Runtime 库可移植加载）---
# 必须在 ldd / 启动之前导出，否则会命中开发机绝对 RUNPATH 或报缺库。
export LD_LIBRARY_PATH="$APP_DIR/lib:${LD_LIBRARY_PATH:-}"
if [ "$(uname -s)" = "Darwin" ]; then
  export DYLD_LIBRARY_PATH="$APP_DIR/lib:${DYLD_LIBRARY_PATH:-}"
fi

# --- 0.1 自动加载本机密钥文件（单机生产的 env 文件模式）---
# 约定两个 chmod 600 文件放在部署用户 $HOME（由 scripts/setup-prod-deps.sh 维护）：
#   ~/.aegisgate_prod_creds  —— PG 连接串(POSTGRES_URL / PG_PASSWORD，脚本自动生成)
#   ~/.aegisgate_app_secrets —— 网关/管理台/模型供应商 Key(你自行编辑，重跑 setup 不覆盖)
# 存在即 source（set -a 让无 export 的赋值也导出）；权限宽于 600 时告警，避免同机
# 其他用户读到明文密钥。注意：文件中的赋值会覆盖当前环境同名变量（模板里的空占位
# 会清掉此前手动 export 的值），故请以这两个密钥文件为单一来源，不要与手动 export 混用。
load_secret_file() {
  local f="$1" perm=""
  [ -f "$f" ] || return 0
  if [ "$(uname -s)" = "Darwin" ]; then
    perm=$(stat -f '%Lp' "$f" 2>/dev/null || echo "")
  else
    perm=$(stat -c '%a' "$f" 2>/dev/null || echo "")
  fi
  if [ -n "$perm" ] && [ "$perm" != "600" ]; then
    echo "  ⚠ 密钥文件 $f 权限为 $perm，建议 chmod 600（防同机其他用户读取明文）"
  fi
  set -a
  # shellcheck disable=SC1090
  . "$f"
  set +a
}
load_secret_file "$HOME/.aegisgate_prod_creds"
load_secret_file "$HOME/.aegisgate_app_secrets"

ERRORS=0

# --- 0.1 档位 + 模式解析（TASK-20260622-01 E3 / G2）---
# 档位决定使用哪份配置：community（默认 / memory+sqlite）或 prod（redis+pg+otel+guard）。
# 优先级：--profile 旗标 > AEGISGATE_PROFILE env > 默认 community（向后兼容）。
PROFILE="${AEGISGATE_PROFILE:-community}"
MODE="--daemon"
while [ $# -gt 0 ]; do
  case "$1" in
    --profile) PROFILE="${2:-community}"; shift 2 ;;
    -f|--foreground) MODE="--foreground"; shift ;;
    --daemon) MODE="--daemon"; shift ;;
    *) shift ;;  # 忽略未知参数（向后兼容旧调用）
  esac
done
if [ "$PROFILE" = "prod" ] || [ "$PROFILE" = "production" ]; then
  CONFIG_FILE="config/aegisgate.prod.yaml"
else
  CONFIG_FILE="config/aegisgate.yaml"
fi

# --- 0.5 首启 seeding（升级安全）---
# 包内只含 *.example 模板；真实 config 仅在缺失时从模板生成，已存在则保留，
# 因此原地解压升级永不覆盖用户自定义的 config。
seed_config() {
  local example="$1" real="${1%.example}"
  if [ -f "$example" ] && [ ! -f "$real" ]; then
    cp "$example" "$real"
    echo "  ✓ 首次部署：已从模板生成 $real"
  fi
}
seed_config config/aegisgate.yaml.example
seed_config config/models.yaml.example
# prod 档位：额外 seeding 生产模板（仅当包内携带 .example 时）
if [ "$PROFILE" = "prod" ] || [ "$PROFILE" = "production" ]; then
  seed_config config/aegisgate.prod.yaml.example
fi
if [ -d config/rules ]; then
  for ex in config/rules/*.yaml.example; do
    [ -e "$ex" ] && seed_config "$ex"
  done
fi

# --- 1. Check files ---
echo "[检查] 运行环境..."

if [ ! -x ./aegisgate ]; then
  echo "  ✗ 缺少可执行文件: aegisgate"
  ERRORS=$((ERRORS + 1))
else
  echo "  ✓ aegisgate"
fi

if [ ! -f "$CONFIG_FILE" ]; then
  echo "  ✗ 缺少配置文件: $CONFIG_FILE"
  ERRORS=$((ERRORS + 1))
else
  echo "  ✓ $CONFIG_FILE (profile=$PROFILE)"
fi

if [ ! -f config/models.yaml ]; then
  echo "  ✗ 缺少配置文件: config/models.yaml"
  ERRORS=$((ERRORS + 1))
else
  echo "  ✓ config/models.yaml"
fi

# --- 2. Check shared libraries (跨平台: Linux ldd / macOS otool -L) ---
if [ "$(uname -s)" = "Darwin" ]; then
  MISSING_LIBS=$(otool -L ./aegisgate 2>/dev/null | grep -i "not found" || true)
else
  MISSING_LIBS=$(ldd ./aegisgate 2>/dev/null | grep "not found" || true)
fi
if [ -n "$MISSING_LIBS" ]; then
  echo "  ✗ 缺少动态库:"
  echo "$MISSING_LIBS" | sed 's/^/      /'
  ERRORS=$((ERRORS + 1))
else
  echo "  ✓ 动态库依赖完整 (ONNX Runtime 从 lib/ 加载)"
fi

# --- 2.5 Check ONNX models (非阻塞: 缺失仅提示下载，不阻止启动) ---
# ONNX 是基础能力，但模型文件 (~832MB) 不随包分发，首次部署需下载。
EMBED_MODEL="models/bge-small-zh-v1.5.onnx"
if [ ! -f "$EMBED_MODEL" ]; then
  echo "  ⚠ 语义缓存模型缺失: $EMBED_MODEL"
  echo "      下载: ./scripts/download_model.sh models"
else
  echo "  ✓ 语义缓存模型 ($EMBED_MODEL)"
fi

GUARD_MODEL=$(ls models/guard/*.onnx 2>/dev/null | head -1 || true)
if [ -z "$GUARD_MODEL" ]; then
  echo "  ⚠ Guard 注入检测模型缺失: models/guard/*.onnx"
  echo "      下载: ./scripts/download_guard_model.sh models/guard"
  echo "      下载后在 config/aegisgate.yaml 设 security.guard_model.enabled: true"
else
  echo "  ✓ Guard 模型 ($GUARD_MODEL)"
fi

# --- 3. Check env vars ---
if [ -z "${AEGISGATE_API_KEY:-}" ]; then
  echo "  ✗ 未设置 AEGISGATE_API_KEY"
  ERRORS=$((ERRORS + 1))
else
  echo "  ✓ AEGISGATE_API_KEY"
fi

if [ -z "${AEGISGATE_ADMIN_JWT_SECRET:-}" ]; then
  echo "  ⚠ 未设置 AEGISGATE_ADMIN_JWT_SECRET (Web 管理面板将无法登录)"
fi

# Check if at least one provider key is set
HAS_PROVIDER=false
for var in DEEPSEEK_API_KEY OPENAI_API_KEY CLAUDE_API_KEY QWEN_API_KEY GEMINI_API_KEY DOUBAO_API_KEY MISTRAL_API_KEY; do
  if [ -n "${!var:-}" ]; then
    HAS_PROVIDER=true
    echo "  ✓ $var"
  fi
done
if [ "$HAS_PROVIDER" = false ]; then
  echo "  ⚠ 未设置任何模型供应商 Key (请求将无法转发到上游)"
fi

# --- 4. Abort on errors ---
if [ "$ERRORS" -gt 0 ]; then
  echo ""
  echo "发现 ${ERRORS} 个错误，无法启动。"
  echo ""
  echo "用法:"
  echo "  export AEGISGATE_API_KEY=\"your-gateway-key\""
  echo "  export DEEPSEEK_API_KEY=\"sk-...\"    # 或其他供应商"
  echo "  ./start.sh"
  exit 1
fi

# --- 5. Start ---
mkdir -p data logs
echo ""
echo "========================================"
echo " AegisGate"
echo " Profile: $PROFILE"
echo " Config:  $CONFIG_FILE"
echo " Data:    data/"
echo " Logs:    logs/aegisgate.log"
echo " API:     http://localhost:8080/v1/chat/completions"
echo " Admin:   http://localhost:8080/admin/"
echo " Health:  http://localhost:8080/health"
echo "========================================"
echo ""

if [ "$MODE" = "--foreground" ]; then
  echo "前台模式启动 (Ctrl+C 停止)"
  echo ""
  ./aegisgate "$CONFIG_FILE" &
  AEGIS_PID=$!
  echo "$AEGIS_PID" > "$APP_DIR/aegisgate.pid"
  trap 'rm -f "$APP_DIR/aegisgate.pid"' EXIT
  wait "$AEGIS_PID" 2>/dev/null || true
else
  nohup ./aegisgate "$CONFIG_FILE" >> logs/console.log 2>&1 &
  AEGIS_PID=$!
  echo "$AEGIS_PID" > "$APP_DIR/aegisgate.pid"

  sleep 1
  if kill -0 "$AEGIS_PID" 2>/dev/null; then
    echo "AegisGate 已启动 (PID: $AEGIS_PID)"
    echo ""
    echo " 查看日志:  tail -f logs/aegisgate.log"
    echo " 停止服务:  ./stop.sh"
    echo " 热重载:    ./reload.sh"
  else
    echo "启动失败，查看日志:"
    echo "  cat logs/console.log"
    rm -f "$APP_DIR/aegisgate.pid"
    exit 1
  fi
fi
STARTUP
chmod +x "$DIST_DIR/start.sh"

# Stop script
cat > "$DIST_DIR/stop.sh" << 'STOPSH'
#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="$APP_DIR/aegisgate.pid"

if [ ! -f "$PID_FILE" ]; then
  # Fallback: find by process name
  PID=$(pgrep -f './aegisgate' 2>/dev/null | head -1 || true)
  if [ -z "$PID" ]; then
    echo "AegisGate 未在运行"
    exit 0
  fi
else
  PID=$(cat "$PID_FILE")
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "PID $PID 已不存在，清理 pid 文件"
    rm -f "$PID_FILE"
    exit 0
  fi
fi

echo "正在停止 AegisGate (PID: $PID)..."
kill -TERM "$PID"

# Wait up to 30s for graceful shutdown
for i in $(seq 1 30); do
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "AegisGate 已停止 (${i}s)"
    rm -f "$PID_FILE"
    exit 0
  fi
  sleep 1
done

echo "优雅停止超时，强制终止..."
kill -9 "$PID" 2>/dev/null || true
rm -f "$PID_FILE"
echo "AegisGate 已强制停止"
STOPSH
chmod +x "$DIST_DIR/stop.sh"

# Reload script
cat > "$DIST_DIR/reload.sh" << 'RELOADSH'
#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="$APP_DIR/aegisgate.pid"

if [ ! -f "$PID_FILE" ]; then
  PID=$(pgrep -f './aegisgate' 2>/dev/null | head -1 || true)
else
  PID=$(cat "$PID_FILE")
fi

if [ -z "${PID:-}" ] || ! kill -0 "$PID" 2>/dev/null; then
  echo "AegisGate 未在运行"
  exit 1
fi

echo "发送 SIGHUP 热重载配置 (PID: $PID)..."
kill -HUP "$PID"
echo "已发送，查看日志确认重载结果"
RELOADSH
chmod +x "$DIST_DIR/reload.sh"

# README
cat > "$DIST_DIR/README.txt" << 'README'
AegisGate — 高性能 AI 网关代理
========================================

快速启动:

  1. 设置环境变量:
     export AEGISGATE_API_KEY="your-gateway-key"
     export DEEPSEEK_API_KEY="sk-..."

  2. 启动:
     ./start.sh

  3. 健康检查:
     curl http://localhost:8080/health

  4. 发送请求:
     curl -X POST http://localhost:8080/v1/chat/completions \
       -H "Content-Type: application/json" \
       -H "Authorization: Bearer $AEGISGATE_API_KEY" \
       -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"你好"}]}'

  5. Web 管理面板 (需设置 AEGISGATE_ADMIN_JWT_SECRET):
     浏览器访问 http://localhost:8080/admin/

ONNX 模型 (基础能力):
  本包已内置 ONNX Runtime 动态库 (lib/)，start.sh 会自动设置
  LD_LIBRARY_PATH 加载，无需手动安装。模型文件因体积较大未随包分发，
  首次部署请下载:

  - 语义缓存 embedding 模型 (默认启用):
      ./scripts/download_model.sh models

  - Guard 提示注入检测模型 (默认关闭，下载后手动开启):
      ./scripts/download_guard_model.sh models/guard
      然后编辑 config/aegisgate.yaml:
        security:
          guard_model:
            enabled: true

  模型缺失不会阻止启动 (graceful 降级)，start.sh 仅打印下载提示。

常用操作:
  ./start.sh             启动 (后台运行，日志写 logs/)
  ./start.sh -f          前台启动 (日志输出到终端，Ctrl+C 停止)
  ./stop.sh              优雅停止 (SIGTERM，等待 30s)
  ./reload.sh            热重载配置 (SIGHUP，不中断服务)
  tail -f logs/aegisgate.log   查看实时日志

文件说明:
  aegisgate              网关主程序
  aegisctl               CLI 管理工具
  start.sh               启动脚本 (含环境检查)
  stop.sh                停止脚本 (优雅关闭)
  reload.sh              热重载脚本 (不中断服务)
  config/                配置: *.example 为出厂模板, start.sh 首启自动生成同名真实配置
  lib/                   ONNX Runtime 动态库 (start.sh 自动加载)
  scripts/               模型下载脚本 (download_model.sh / download_guard_model.sh)
  models/                ONNX 模型目录 (需下载, 运行时读取)
  web/admin/dist/        Web 管理面板
  data/                  数据目录 (SQLite, 运行时生成)
  logs/                  日志目录 (运行时生成)

升级 (解压即替换, 零顾虑):
  直接把新版本解压覆盖到当前部署目录即可, 无需先删除旧目录:

    tar xzf aegisgate-<新版本>-<os>-<arch>.tar.gz --strip-components=1 -C <部署目录>

  - 会更新: aegisgate / aegisctl / lib/ / web/ / scripts/ / start.sh 等程序文件,
    以及 config/*.example 出厂模板。
  - 不会动: 你的 config/*.yaml 与 config/rules/*.yaml (包内只有 *.example, 真实配置
    由 start.sh 首启生成), 以及 data/ / logs/ / models/ (本就不随包分发)。
  - 想对照新版默认配置: 比较 config/aegisgate.yaml 与 config/aegisgate.yaml.example。

文档: https://github.com/privonyx/loong-aegisgate
README

echo "  ✓ 分发包组装完成"

# --- Archive ---
echo "[2/2] 创建压缩包..."
cd dist
tar czf "${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
cd "$ROOT_DIR"

SIZE=$(du -sh "dist/${PACKAGE_NAME}.tar.gz" | cut -f1)
echo ""
echo "========================================"
echo " 打包完成!"
echo ""
echo " 产出: dist/${PACKAGE_NAME}.tar.gz (${SIZE})"
echo ""
echo " 使用方式:"
echo "   tar xzf ${PACKAGE_NAME}.tar.gz"
echo "   cd ${PACKAGE_NAME}"
echo "   export AEGISGATE_API_KEY=\"your-key\""
echo "   ./start.sh"
echo "========================================"
