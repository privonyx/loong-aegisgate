cmake_minimum_required(VERSION 3.20)
project(aegisgate VERSION 1.0.0 LANGUAGES CXX)

# 构建脚本可用 -DAEGIS_VERSION=x.y.z 覆盖编入二进制的版本，使发布标签(-v)与
# tar 包名、启动横幅、AEGISGATE_VERSION 宏全程一致；留空则沿用上面 project() 的默认。
# 仅取前缀的数字段填充 MAJOR/MINOR/PATCH（version.h.in 需要纯整数），原始串作 PROJECT_VERSION。
if(DEFINED AEGIS_VERSION AND NOT AEGIS_VERSION STREQUAL "")
    set(PROJECT_VERSION "${AEGIS_VERSION}")
    set(aegisgate_VERSION "${AEGIS_VERSION}")
    set(PROJECT_VERSION_MAJOR 0)
    set(PROJECT_VERSION_MINOR 0)
    set(PROJECT_VERSION_PATCH 0)
    if(AEGIS_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
        set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
        set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    endif()
    message(STATUS "版本覆盖: AEGISGATE_VERSION=${PROJECT_VERSION}（来自 -DAEGIS_VERSION）")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Werror)

option(ENABLE_ONNX "Enable ONNX Runtime for neural embeddings" ON)
option(ENABLE_GUARD_MODEL "Enable ONNX guard model for safety classification" ON)
option(ENABLE_REDIS "Enable Redis cache backend (hiredis)" OFF)
option(ENABLE_PG "Enable PostgreSQL persistent backend (libpq)" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_COVERAGE "Enable code coverage" OFF)
option(ENABLE_OPENTELEMETRY "Enable OpenTelemetry distributed tracing" OFF)
option(ENABLE_CURL "Enable CurlUpstreamClient for true streaming" ON)
option(ENABLE_MILVUS "Enable Milvus vector store backend" OFF)
option(ENABLE_QDRANT "Enable Qdrant vector store backend" OFF)
option(ENABLE_CONTROL_PLANE "Build control plane gRPC server + aegisctl config subcommands (Phase 9.3)" OFF)

if(ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()
if(ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

# hnswlib 的 SSE/AVX 距离核在 searchBaseLayer 里对 *(neighbor + j + 1) 做
# _mm_prefetch，最后一个邻居时会读到上层 link list 末尾后一个元素。该越界对
# 硬件 prefetch 无害，但 AddressSanitizer 会插桩这次 load 并判定 heap-buffer-
# overflow（CacheMigratorTest.LargeIndexDumps 在 debug-asan 矩阵下 abort）。
# 任一 sanitizer 构建下关闭 hnswlib 手动向量化以消除该越界读；Release 构建保留
# SSE/AVX 全速距离计算，不受影响。
if(ENABLE_ASAN OR ENABLE_TSAN OR ENABLE_UBSAN)
    add_compile_definitions(NO_MANUAL_VECTORIZATION)
endif()

include(cmake/Dependencies.cmake)

if(ENABLE_CURL)
    find_package(CURL REQUIRED)
endif()

if(ENABLE_CONTROL_PLANE)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)
    add_compile_definitions(AEGISGATE_ENABLE_CONTROL_PLANE=1)
    message(STATUS "Control plane enabled: gRPC ${gRPC_VERSION}, Protobuf ${Protobuf_VERSION}")
endif()

add_subdirectory(src)

if(ENABLE_CONTROL_PLANE)
    add_subdirectory(src/control_plane)
endif()

option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

add_executable(aegisctl src/cli/aegisctl.cpp)
target_include_directories(aegisctl PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aegisctl PRIVATE aegisgate_core Drogon::Drogon nlohmann_json::nlohmann_json)
if(ENABLE_CONTROL_PLANE)
    # Epic 7: add the gRPC-aware config subcommand suite. The static
    # library already pulls in control_plane_proto + gRPC::grpc++, so no
    # further link flags are needed here.
    target_link_libraries(aegisctl PRIVATE aegisctl_controlplane)
endif()

# Web admin panel build target
find_program(NPM npm)
if(NPM)
    add_custom_target(web_admin
        COMMAND ${NPM} run build
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/web/admin
        COMMENT "Building admin panel frontend"
    )
endif()

# 构建能力摘要：在 configure 末尾打印各能力档位，让使用者一眼确认自己构建的是
# 哪个档位（尤其 ONNX/Guard 可能因依赖缺失在 cmake/Dependencies.cmake 中被
# graceful 降级回 OFF，此处反映的是降级后的真实状态）。
function(_aegis_capability_row label flag)
    if(${flag})
        message(STATUS "  [ON ] ${label}")
    else()
        message(STATUS "  [off] ${label}")
    endif()
endfunction()

message(STATUS "")
message(STATUS "================ AegisGate 构建能力摘要 ================")
message(STATUS "  版本:    ${PROJECT_VERSION}")
message(STATUS "  构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "  -- 核心能力 --")
_aegis_capability_row("ONNX 神经 embedding (ENABLE_ONNX)" ENABLE_ONNX)
_aegis_capability_row("ONNX 安全护栏模型 (ENABLE_GUARD_MODEL)" ENABLE_GUARD_MODEL)
_aegis_capability_row("CURL 真流式上游 (ENABLE_CURL)" ENABLE_CURL)
message(STATUS "  -- 生产基础设施 --")
_aegis_capability_row("Redis 共享缓存 (ENABLE_REDIS)" ENABLE_REDIS)
_aegis_capability_row("PostgreSQL 持久化 (ENABLE_PG)" ENABLE_PG)
_aegis_capability_row("OpenTelemetry 分布式追踪 (ENABLE_OPENTELEMETRY)" ENABLE_OPENTELEMETRY)
_aegis_capability_row("gRPC 控制面 (ENABLE_CONTROL_PLANE)" ENABLE_CONTROL_PLANE)
message(STATUS "  -- 向量检索后端 --")
_aegis_capability_row("Milvus (ENABLE_MILVUS)" ENABLE_MILVUS)
_aegis_capability_row("Qdrant (ENABLE_QDRANT)" ENABLE_QDRANT)
message(STATUS "  -- 开发/诊断 --")
_aegis_capability_row("AddressSanitizer (ENABLE_ASAN)" ENABLE_ASAN)
_aegis_capability_row("ThreadSanitizer (ENABLE_TSAN)" ENABLE_TSAN)
_aegis_capability_row("UBSanitizer (ENABLE_UBSAN)" ENABLE_UBSAN)
_aegis_capability_row("代码覆盖率 (ENABLE_COVERAGE)" ENABLE_COVERAGE)
_aegis_capability_row("测试 (BUILD_TESTS)" BUILD_TESTS)
_aegis_capability_row("基准 (BUILD_BENCHMARKS)" BUILD_BENCHMARKS)
message(STATUS "=======================================================")
message(STATUS "")
