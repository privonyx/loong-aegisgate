find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(GTest REQUIRED)
find_package(re2 REQUIRED)
find_package(hnswlib REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-sqlite3 REQUIRED)

if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)
endif()

if(ENABLE_ONNX OR ENABLE_GUARD_MODEL)
    # 跨平台 ONNX Runtime 获取（决策 A3 / 见
    # memory-bank/creative/creative-onnx-cross-platform-acquisition.md）。
    # 决策序：
    #   1) 显式 -DONNXRUNTIME_ROOT=<dir> 优先（用户明确意图 → REQUIRED，SR-2 豁免）
    #   2) 平台默认预编译目录 third_party/onnxruntime-<os>-<arch>-<ver>
    #      （Linux x64 命中现有 vendored 包 → 零回归）
    #   3) vcpkg find_package(onnxruntime CONFIG) 回退
    # 三条路径统一导出 target onnxruntime::onnxruntime，故 src/CMakeLists.txt 无需改动。
    #
    # graceful 默认 ON（TASK-20260614-01 / SR-2）：ONNX 现在是默认基础能力
    # （CMakeLists.txt option 默认 ON）。当探测不到 ONNX Runtime 时，必须 graceful
    # 降级 ENABLE_ONNX + ENABLE_GUARD_MODEL（两者都依赖 onnxruntime）回 OFF，而不是
    # REQUIRED 硬失败——否则缺预编译包的平台（如未跑 fetch-onnxruntime.sh 的 macOS）
    # 默认开启会让 cmake configure 直接崩溃。降级必须打印醒目 WARNING（SR-1：诚实可见，
    # 非静默），让部署者知道 ONNX 能力未实际启用。缺失时可运行
    # scripts/fetch-onnxruntime.sh 按平台下载（含 sha256 校验）。
    set(ONNXRUNTIME_VERSION "1.24.2")
    set(_ort_found FALSE)
    if(DEFINED ONNXRUNTIME_ROOT AND EXISTS "${ONNXRUNTIME_ROOT}")
        list(APPEND CMAKE_PREFIX_PATH "${ONNXRUNTIME_ROOT}")
        # 用户显式指定路径 → REQUIRED（明确意图，SR-2 合法豁免）
        find_package(onnxruntime REQUIRED)
        set(_ort_found TRUE)
        message(STATUS "ONNX Runtime from ONNXRUNTIME_ROOT=${ONNXRUNTIME_ROOT}")
    else()
        set(_ort_os "linux")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_ort_arch "arm64")
        else()
            set(_ort_arch "x64")
        endif()
        set(_ort_dir "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-${_ort_os}-${_ort_arch}-${ONNXRUNTIME_VERSION}")
        if(EXISTS "${_ort_dir}")
            list(APPEND CMAKE_PREFIX_PATH "${_ort_dir}")
            find_package(onnxruntime CONFIG QUIET)
            if(onnxruntime_FOUND)
                set(_ort_found TRUE)
                message(STATUS "ONNX Runtime from prebuilt: ${_ort_dir}")
            endif()
        else()
            # vcpkg 回退（QUIET，不硬失败 → 让下方 graceful 降级处理）
            find_package(onnxruntime CONFIG QUIET)
            if(onnxruntime_FOUND)
                set(_ort_found TRUE)
                message(STATUS "ONNX Runtime from vcpkg find_package(onnxruntime CONFIG)")
            endif()
        endif()
    endif()

    if(NOT _ort_found)
        # SR-1 / SR-2：探测失败 → graceful 降级（非 REQUIRED 硬失败）+ 醒目 WARNING
        message(WARNING
            "ONNX Runtime not found — gracefully disabling ENABLE_ONNX and "
            "ENABLE_GUARD_MODEL. Neural embeddings and the ONNX safety guard "
            "will NOT be active. Run scripts/fetch-onnxruntime.sh to download a "
            "prebuilt package, or set -DONNXRUNTIME_ROOT=<dir> to enable them.")
        set(ENABLE_ONNX OFF CACHE BOOL "Enable ONNX Runtime for neural embeddings" FORCE)
        set(ENABLE_GUARD_MODEL OFF CACHE BOOL "Enable ONNX guard model for safety classification" FORCE)
    else()
        if(ENABLE_ONNX)
            message(STATUS "ONNX Runtime enabled — neural embeddings available")
        endif()
        if(ENABLE_GUARD_MODEL AND NOT ENABLE_ONNX)
            message(STATUS "ONNX Runtime enabled — guard model only")
        endif()
    endif()
endif()

if(ENABLE_GUARD_MODEL)
    # vcpkg's sentencepiece port exposes pkg-config metadata rather than a
    # CMake config package. SentencePiece lives behind the `guard-spm` vcpkg
    # feature (NOT a default dependency), so standard builds without
    # -DVCPKG_MANIFEST_FEATURES=guard-spm will not have it installed.
    #
    # graceful 默认 ON（TASK-20260614-01 / SR-1 / SR-2）：ENABLE_GUARD_MODEL 默认 ON，
    # 但若 SentencePiece 探测不到（标准构建无 guard-spm feature），graceful 降级
    # *仅* ENABLE_GUARD_MODEL 回 OFF（ENABLE_ONNX embedding 不受影响，独立保留），并打印
    # 醒目 WARNING 提示如何启用 guard tokenizer。探测用 QUIET 而非 REQUIRED（SR-2）。
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SENTENCEPIECE QUIET IMPORTED_TARGET sentencepiece)
    if(SENTENCEPIECE_FOUND)
        message(STATUS "SentencePiece enabled — guard tokenizer available")
    else()
        message(WARNING
            "SentencePiece not found — gracefully disabling ENABLE_GUARD_MODEL "
            "(ENABLE_ONNX neural embeddings remain enabled). The ONNX safety "
            "guard will NOT be active. Reconfigure with "
            "-DVCPKG_MANIFEST_FEATURES=guard-spm to install SentencePiece and "
            "enable the guard tokenizer.")
        set(ENABLE_GUARD_MODEL OFF CACHE BOOL "Enable ONNX guard model for safety classification" FORCE)
    endif()
endif()

if(ENABLE_REDIS)
    find_package(hiredis CONFIG REQUIRED)
    message(STATUS "Redis enabled — hiredis cache backend available")
endif()

if(ENABLE_PG)
    find_package(PostgreSQL REQUIRED)
    message(STATUS "PostgreSQL enabled — libpq persistent backend available")
endif()

if(ENABLE_OPENTELEMETRY)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(CURL CONFIG REQUIRED)
    find_package(opentelemetry-cpp CONFIG REQUIRED)
    message(STATUS "OpenTelemetry enabled — distributed tracing available")
endif()
