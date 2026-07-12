#!/usr/bin/env bash
set -euo pipefail

# AegisGate 统一构建脚本
#
# 一个脚本覆盖「生产版」与「调试版」，由构建类型自动选择能力档位：
#   Release / RelWithDebInfo → 生产全能力版
#       Redis 共享缓存 + PostgreSQL 持久化 + OpenTelemetry 分布式追踪 + gRPC 控制面
#   Debug                    → 轻量调试版（不带上述生产基础设施，编译更快）
#
# 含 gcc-13 回退、vcpkg 探测、版本注入、前端构建，并可选打分发 tar 包。
#
# 用法: ./scripts/build.sh [-t TYPE] [-v VERSION] [-p ARCH] [-P] [-d]
#                          [--backend|--frontend|--all] [-h]
#
#   -t, --type TYPE     构建类型 (Release|Debug|RelWithDebInfo)，默认 Release
#                       Release/RelWithDebInfo=生产档位；Debug=调试档位
#   -v, --version VER   版本标签 (如 V1.0.3)，决定分发包名与编入二进制的版本(去前缀 V)
#                       默认取自 CMakeLists.txt
#   -p, --arch ARCH     目标架构标签 (如 x86_64|arm64)，影响分发包名；默认本机 uname -m
#   -P, --package       构建完成后打分发包 (dist/*.tar.gz)
#   -d, --clean         删除编译目录 build/；单独使用时清理后即退出，
#                       与构建/打包选项同用则先清理再全新构建
#       --backend       仅编译 C++ 后端 (aegisgate + aegisctl)
#       --frontend      仅构建 Web 管理面板
#       --all           编译后端 + 前端 (默认)
#   -h, --help          显示帮助
#
# 环境变量:
#   VCPKG_ROOT            vcpkg 安装路径
#   CC / CXX              指定编译器（覆盖自动选择;默认 GCC>=15 时自动回退 gcc-13/g++-13）
#   CMAKE_EXTRA_ARGS      追加的 CMake 参数（在档位默认值之后，可覆盖之）
#   AEGIS_VCPKG_FEATURES  vcpkg manifest features（覆盖档位默认值）
#
# 示例:
#   ./scripts/build.sh                              # 生产版，编译后端+前端
#   ./scripts/build.sh -t Debug --backend          # 调试版，仅后端
#   ./scripts/build.sh -t Release -v V1.0.3 -p x86_64 -P   # 生产版并打包
#   ./scripts/build.sh -d                           # 仅删除 build/ 编译目录
#   ./scripts/build.sh -d --backend                 # 清理后全新编译后端

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() {
  sed -n '4,40p' "$0" | sed 's/^# \{0,1\}//'
}

BUILD_TYPE_OPT=""
VERSION_OPT=""
ARCH_OPT=""
DO_PACKAGE=false
DO_CLEAN=false
BUILD_BACKEND=false
BUILD_FRONTEND=false
TARGET_SET=false

while [ $# -gt 0 ]; do
  case "$1" in
    -t|--type)
      [ $# -ge 2 ] || { echo "错误: 选项 $1 需要一个参数" >&2; exit 1; }
      BUILD_TYPE_OPT="$2"; shift 2 ;;
    -v|--version)
      [ $# -ge 2 ] || { echo "错误: 选项 $1 需要一个参数" >&2; exit 1; }
      VERSION_OPT="$2"; shift 2 ;;
    -p|--arch)
      [ $# -ge 2 ] || { echo "错误: 选项 $1 需要一个参数" >&2; exit 1; }
      ARCH_OPT="$2"; shift 2 ;;
    -P|--package)  DO_PACKAGE=true; shift ;;
    -d|--clean)    DO_CLEAN=true; shift ;;
    --backend)     BUILD_BACKEND=true;  TARGET_SET=true; shift ;;
    --frontend)    BUILD_FRONTEND=true; TARGET_SET=true; shift ;;
    --all)         BUILD_BACKEND=true; BUILD_FRONTEND=true; TARGET_SET=true; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "未知参数: $1" >&2; usage; exit 1 ;;
  esac
done

cd "$ROOT_DIR"

# 清理：删除编译目录。单独使用 -d（未叠加任何构建/打包动作）时清理后即退出，
# 与构建目标 / -P 同用时先清理，再走下面的全新构建流程。
if [ "$DO_CLEAN" = true ]; then
  echo "[Clean] 删除编译目录 build/ ..."
  rm -rf build
  echo "  ✓ 已删除 build/"
  if [ "$TARGET_SET" = false ] && [ "$DO_PACKAGE" = false ]; then
    exit 0
  fi
  echo ""
fi

# 未显式指定构建目标时默认编译后端+前端（分发包需要 Web 管理面板）
if [ "$TARGET_SET" = false ]; then
  BUILD_BACKEND=true
  BUILD_FRONTEND=true
fi

# 构建类型：-t 优先，其次 CMAKE_BUILD_TYPE 环境变量，默认 Release
BUILD_TYPE="${BUILD_TYPE_OPT:-${CMAKE_BUILD_TYPE:-Release}}"

# 能力档位由构建类型决定：仅 Debug 走轻量调试档；其余（Release/RelWithDebInfo）走生产档。
if [ "$BUILD_TYPE" = "Debug" ]; then
  PROFILE="debug"
else
  PROFILE="prod"
fi

# 版本：-v 去掉前缀 V/v 后驱动编入二进制的版本；否则沿用已有 AEGIS_VERSION 或 CMakeLists。
[ -n "$VERSION_OPT" ] && AEGIS_VERSION="${VERSION_OPT#[Vv]}"
export AEGIS_VERSION="${AEGIS_VERSION:-}"
VERSION="$AEGIS_VERSION"
if [ -z "$VERSION" ]; then
  VERSION=$(grep -oP 'project\(aegisgate VERSION \K[0-9.]+' CMakeLists.txt 2>/dev/null | head -1 || true)
  VERSION="${VERSION:-0.0.0}"
fi

# 生产档位：叠加生产能力开关与对应 vcpkg features（用户可经环境变量覆盖）。
if [ "$PROFILE" = "prod" ]; then
  PROD_CMAKE_ARGS="-DENABLE_REDIS=ON -DENABLE_PG=ON -DENABLE_OPENTELEMETRY=ON -DENABLE_CONTROL_PLANE=ON"
  CMAKE_EXTRA_ARGS="${PROD_CMAKE_ARGS}${CMAKE_EXTRA_ARGS:+ ${CMAKE_EXTRA_ARGS}}"
  # 生产能力需要的 vcpkg manifest features 必须随之开启，否则 cmake 的 REQUIRED
  # 查找（hiredis / libpq / grpc）会失败。onnxruntime 不在当前 vcpkg baseline
  # （见 scripts/fetch-onnxruntime.sh）；SentencePiece 经 guard-spm。调用者可用
  # AEGIS_VCPKG_FEATURES 覆盖。
  AEGIS_VCPKG_FEATURES="${AEGIS_VCPKG_FEATURES:-guard-spm;redis;pg;otel;control-plane}"
  # 开源后 third_party/onnxruntime-* 不再入库；生产档位需要 ENABLE_GUARD_MODEL=ON
  # 时先拉取官方预编译包（已存在则跳过）。
  if ! compgen -G "third_party/onnxruntime-*" > /dev/null; then
    echo "[Backend] 拉取 ONNX Runtime 预编译包..."
    bash "$SCRIPT_DIR/fetch-onnxruntime.sh"
  fi
fi

echo "========================================"
echo " AegisGate Build v${VERSION}"
echo " Type:    ${BUILD_TYPE}"
if [ "$PROFILE" = "prod" ]; then
  echo " Profile: 生产 (Redis + PostgreSQL + OpenTelemetry + 控制面)"
else
  echo " Profile: 调试 (轻量,无生产基础设施)"
fi
[ "$DO_PACKAGE" = true ] && echo " 打包:    是 (dist/*.tar.gz)"
echo "========================================"
echo ""

# --- Backend ---
if [ "$BUILD_BACKEND" = true ]; then
  echo "[Backend] 编译 C++ 后端..."

  # 生产档位预检：vcpkg 会从源码编译 libpq(PostgreSQL)，其 configure 需要系统
  # autotools + bison/flex。缺失时 vcpkg 会在数分钟构建后才硬失败，故此处提前拦截，
  # 打印 apt 安装命令并退出，避免白等。仅 apt 系发行版 + 生产档位做此检查。
  if [ "$PROFILE" = "prod" ] && command -v apt-get >/dev/null 2>&1; then
    # libpq 源码构建硬依赖 autoconf + bison + flex；libtool/automake 非必需，
    # 仅在安装建议里一并给出（多装无害）。
    MISSING_BUILD_DEPS=()
    for tool in autoconf bison flex; do
      command -v "$tool" >/dev/null 2>&1 || MISSING_BUILD_DEPS+=("$tool")
    done
    if [ "${#MISSING_BUILD_DEPS[@]}" -gt 0 ]; then
      echo "错误: 生产构建需要以下系统工具来编译 libpq，但未安装: ${MISSING_BUILD_DEPS[*]}" >&2
      echo "请先安装后重试:" >&2
      echo "  sudo apt-get install -y autoconf automake libtool autoconf-archive bison flex" >&2
      exit 1
    fi
  fi

  # 编译器选择：优先尊重显式 CC/CXX；否则当系统默认是 GCC >= 15 且本机有 gcc-13 时，
  # 自动回退到 gcc-13/g++-13。当前 vcpkg baseline 锁定的旧 abseil 20240116.2 在 GCC 15
  # 下编译失败（container_memory.h 缺 <cstdint> 的 uintptr_t）。手动 export CC/CXX 可覆盖。
  if [ -z "${CC:-}" ] && [ -z "${CXX:-}" ] \
     && command -v gcc-13 >/dev/null 2>&1 && command -v g++-13 >/dev/null 2>&1; then
    DEFAULT_CC_VER="$(cc --version 2>/dev/null || true)"
    DEFAULT_CC_MAJOR="$(cc -dumpversion 2>/dev/null | cut -d. -f1 || true)"
    if printf '%s' "$DEFAULT_CC_VER" | grep -qi 'free software foundation' \
       && ! printf '%s' "$DEFAULT_CC_VER" | grep -qi 'clang' \
       && [ "${DEFAULT_CC_MAJOR:-0}" -ge 15 ] 2>/dev/null; then
      export CC=gcc-13 CXX=g++-13
      echo "  ⚠ 系统默认 GCC ${DEFAULT_CC_MAJOR} 与当前 vcpkg baseline 的 abseil 不兼容 → 自动改用 gcc-13/g++-13（设 CC/CXX 可覆盖）"
    fi
  fi
  [ -n "${CC:-}" ] && echo "  编译器: CC=$CC CXX=$CXX"

  if [ -z "${VCPKG_ROOT:-}" ]; then
    # macOS 无 /home；搜索 $HOME 与常见安装位置（含 Homebrew /opt/homebrew）
    TOOLCHAIN_FILE=$(find "$HOME" /opt /usr/local /opt/homebrew -path "*/vcpkg/scripts/buildsystems/vcpkg.cmake" 2>/dev/null | head -1 || true)
    if [ -z "$TOOLCHAIN_FILE" ]; then
      echo "错误: 未找到 vcpkg。请设置 VCPKG_ROOT 环境变量"
      exit 1
    fi
    echo "  自动检测 vcpkg: $(dirname "$(dirname "$(dirname "$TOOLCHAIN_FILE")")")"
  else
    TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  fi

  # ONNX 是基础能力（TASK-20260614-01：ENABLE_ONNX / ENABLE_GUARD_MODEL 默认 ON）。
  # onnxruntime 由 third_party/ 预编译包提供（scripts/fetch-onnxruntime.sh；不在
  # 当前 vcpkg baseline）。SentencePiece 经 guard-spm。缺任一依赖时 cmake 会
  # graceful 降级对应开关。默认带 guard-spm；生产档位会在上方自动 fetch ORT。
  # 可用 AEGIS_VCPKG_FEATURES 覆盖（如设为空字符串以构建轻量、无 guard 的版本）。
  VCPKG_FEATURES="${AEGIS_VCPKG_FEATURES-guard-spm}"
  cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTS=OFF \
    ${AEGIS_VERSION:+-DAEGIS_VERSION="$AEGIS_VERSION"} \
    ${VCPKG_FEATURES:+-DVCPKG_MANIFEST_FEATURES="$VCPKG_FEATURES"} \
    ${CMAKE_EXTRA_ARGS:-}
  cmake --build build -j"$(nproc)"

  echo "  ✓ aegisgate  → build/src/aegisgate"
  [ -f build/aegisctl ] && echo "  ✓ aegisctl   → build/aegisctl"
  echo ""
fi

# --- Frontend ---
if [ "$BUILD_FRONTEND" = true ]; then
  echo "[Frontend] 构建 Web 管理面板..."

  if [ ! -f web/admin/package.json ]; then
    echo "  ⚠ 未找到 web/admin/package.json，跳过"
  else
    cd web/admin
    if [ ! -d node_modules ]; then
      echo "  安装依赖..."
      npm install --no-audit --no-fund
    fi
    npx tsc -b && npx vite build
    cd "$ROOT_DIR"
    echo "  ✓ dist → web/admin/dist/"
    echo ""
  fi
fi

echo "========================================"
echo " 构建完成!"
echo "========================================"

# --- Package (可选) ---
if [ "$DO_PACKAGE" = true ]; then
  echo ""
  echo "[Package] 打分发包..."
  # 版本/架构标签透传给 package.sh 用于 tar 包命名（保留前缀 V）。编入二进制的版本
  # 已在前面通过 AEGIS_VERSION（去前缀）统一，二者同源不再脱节。
  [ -n "$VERSION_OPT" ] && export PKG_VERSION="$VERSION_OPT"
  [ -n "$ARCH_OPT" ] && export PKG_ARCH="$ARCH_OPT"
  "$SCRIPT_DIR/package.sh"
fi
