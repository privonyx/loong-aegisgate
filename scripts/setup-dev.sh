#!/usr/bin/env bash
set -euo pipefail

# AegisGate Development Environment Setup
#
# One-click script to bootstrap a complete development environment.
# Checks prerequisites, installs vcpkg, configures CMake, builds the
# project, and runs the test suite.
#
# Usage:
#   ./scripts/setup-dev.sh [OPTIONS]
#
# Options:
#   --skip-tests        Skip running the test suite after build
#   --skip-build        Only install dependencies and configure (no build)
#   --clean             Remove existing build directory before starting
#   --vcpkg-root PATH   Use an existing vcpkg installation at PATH
#   -j N                Number of parallel build jobs (default: nproc)
#   -h, --help          Show this help message

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SKIP_TESTS=false
SKIP_BUILD=false
CLEAN_BUILD=false
VCPKG_ROOT_OVERRIDE=""
BUILD_JOBS=""

# --- Parse arguments ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-tests)    SKIP_TESTS=true; shift ;;
        --skip-build)    SKIP_BUILD=true; shift ;;
        --clean)         CLEAN_BUILD=true; shift ;;
        --vcpkg-root)    VCPKG_ROOT_OVERRIDE="$2"; shift 2 ;;
        -j)              BUILD_JOBS="$2"; shift 2 ;;
        -h|--help)
            head -17 "$0" | tail -12
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Run '$0 --help' for usage."
            exit 1
            ;;
    esac
done

BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

# --- Color output helpers ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
fail()  { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }

# --- Banner ---
echo ""
echo "============================================"
echo "  AegisGate Development Environment Setup"
echo "============================================"
echo ""

# --- Step 1: Check prerequisites ---
info "Checking prerequisites..."

check_cmd() {
    local cmd="$1"
    local min_version="${2:-}"
    local install_hint="${3:-}"

    if ! command -v "$cmd" &>/dev/null; then
        fail "'$cmd' is not installed.${install_hint:+ $install_hint}"
    fi

    if [[ -n "$min_version" ]]; then
        local version
        version=$("$cmd" --version 2>&1 | head -1 | grep -oP '\d+\.\d+(\.\d+)?' | head -1 || echo "0.0.0")
        ok "$cmd found: ${version:-unknown}"
    else
        ok "$cmd found"
    fi
}

check_cmd "cmake" "3.20" "Install: https://cmake.org/install/"
check_cmd "git" "" "Install: sudo apt install git"

# Check for C++ compiler
CXX_COMPILER=""
if command -v g++ &>/dev/null; then
    CXX_COMPILER="g++"
    GCC_VERSION=$(g++ -dumpversion 2>/dev/null || echo "0")
    GCC_MAJOR=$(echo "$GCC_VERSION" | cut -d. -f1)
    if [[ "$GCC_MAJOR" -lt 11 ]]; then
        warn "g++ version $GCC_VERSION detected. GCC 11+ is recommended for C++17 support."
    else
        ok "g++ found: $GCC_VERSION"
    fi
elif command -v clang++ &>/dev/null; then
    CXX_COMPILER="clang++"
    CLANG_VERSION=$(clang++ --version 2>&1 | head -1 | grep -oP '\d+\.\d+(\.\d+)?' | head -1 || echo "0")
    ok "clang++ found: ${CLANG_VERSION:-unknown}"
else
    fail "No C++ compiler found. Install GCC 11+ (sudo apt install g++-11) or Clang 14+."
fi

# Check for optional tools
NINJA_HINT="Install ninja-build for faster builds."
PKGCONFIG_HINT="Install: sudo apt install pkg-config"

if command -v ninja &>/dev/null; then
    ok "ninja found (will use Ninja generator)"
    CMAKE_GENERATOR="-G Ninja"
else
    warn "ninja not found. Using default CMake generator (Make). $NINJA_HINT"
    CMAKE_GENERATOR=""
fi

if command -v pkg-config &>/dev/null; then
    ok "pkg-config found"
else
    warn "pkg-config not found. Some dependencies may not resolve. $PKGCONFIG_HINT"
fi

echo ""

# --- Step 2: Set up vcpkg ---
info "Setting up vcpkg..."

VCPKG_ROOT=""

if [[ -n "$VCPKG_ROOT_OVERRIDE" ]]; then
    if [[ -f "$VCPKG_ROOT_OVERRIDE/scripts/buildsystems/vcpkg.cmake" ]]; then
        VCPKG_ROOT="$VCPKG_ROOT_OVERRIDE"
        ok "Using provided vcpkg: $VCPKG_ROOT"
    else
        fail "Provided vcpkg root '$VCPKG_ROOT_OVERRIDE' does not contain vcpkg.cmake"
    fi
elif [[ -n "${VCPKG_ROOT:-}" && -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]]; then
    ok "Using VCPKG_ROOT from environment: $VCPKG_ROOT"
elif [[ -d "$ROOT_DIR/vcpkg" && -f "$ROOT_DIR/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    VCPKG_ROOT="$ROOT_DIR/vcpkg"
    ok "Using existing vcpkg in project: $VCPKG_ROOT"
else
    info "vcpkg not found. Cloning into $ROOT_DIR/vcpkg ..."
    git clone https://github.com/microsoft/vcpkg.git "$ROOT_DIR/vcpkg"
    info "Bootstrapping vcpkg..."
    "$ROOT_DIR/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
    VCPKG_ROOT="$ROOT_DIR/vcpkg"
    ok "vcpkg installed: $VCPKG_ROOT"
fi

TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
echo ""

# --- Step 3: Create build directory ---
BUILD_DIR="$ROOT_DIR/build"

if [[ "$CLEAN_BUILD" == true && -d "$BUILD_DIR" ]]; then
    info "Removing existing build directory..."
    rm -rf "$BUILD_DIR"
    ok "Clean build directory"
fi

mkdir -p "$BUILD_DIR"

# --- Step 4: Create data directories ---
mkdir -p "$ROOT_DIR/data" "$ROOT_DIR/logs"

# --- Step 5: CMake configure ---
info "Configuring CMake (Debug + Tests)..."

cd "$ROOT_DIR"

cmake -B build \
    $CMAKE_GENERATOR \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTS=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    2>&1 | tail -5

ok "CMake configuration complete"
echo ""

# Symlink compile_commands.json to project root for IDE integration
if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
    ln -sf build/compile_commands.json "$ROOT_DIR/compile_commands.json" 2>/dev/null || true
    ok "Linked compile_commands.json to project root (for IDE integration)"
fi

# --- Step 6: Build ---
if [[ "$SKIP_BUILD" == true ]]; then
    info "Skipping build (--skip-build)"
else
    info "Building AegisGate (Debug, $BUILD_JOBS parallel jobs)..."
    cmake --build build -j"$BUILD_JOBS" 2>&1 | tail -3

    ok "Build complete"

    if [[ -f "$BUILD_DIR/src/aegisgate" ]]; then
        ok "Binary: build/src/aegisgate"
    fi
    if [[ -f "$BUILD_DIR/aegisctl" ]]; then
        ok "CLI:    build/aegisctl"
    fi
    echo ""

    # --- Step 7: Run tests ---
    if [[ "$SKIP_TESTS" == true ]]; then
        info "Skipping tests (--skip-tests)"
    else
        info "Running test suite..."
        cd "$BUILD_DIR"

        if ctest --output-on-failure -j"$BUILD_JOBS" 2>&1 | tail -20; then
            ok "All tests passed"
        else
            warn "Some tests failed. Review the output above for details."
            warn "You can re-run tests with: cd build && ctest --output-on-failure"
        fi

        cd "$ROOT_DIR"
    fi
fi

echo ""

# --- Summary ---
echo "============================================"
echo -e "  ${GREEN}Development environment is ready!${NC}"
echo "============================================"
echo ""
echo "  Project root:   $ROOT_DIR"
echo "  Build directory: $BUILD_DIR"
echo "  vcpkg root:     $VCPKG_ROOT"
echo "  Compiler:       $CXX_COMPILER"
echo ""
echo "  Quick commands:"
echo "    Run gateway:    ./build/src/aegisgate config/aegisgate.yaml"
echo "    Run tests:      cd build && ctest --output-on-failure"
echo "    Rebuild:        cmake --build build -j$BUILD_JOBS"
echo "    Clean rebuild:  ./scripts/setup-dev.sh --clean"
echo ""
echo "  Environment variables to set before running:"
echo "    export AEGISGATE_API_KEY=\"your-api-key\""
echo "    export DEEPSEEK_API_KEY=\"sk-your-key\"  # or other provider"
echo ""
