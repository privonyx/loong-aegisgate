# Phase 9.3 control plane — protoc + grpc_cpp_plugin code generation.
#
# Generates:
#   ${CMAKE_BINARY_DIR}/generated/control_plane/v1/control_plane.pb.{h,cc}
#   ${CMAKE_BINARY_DIR}/generated/control_plane/v1/control_plane.grpc.pb.{h,cc}
#
# and exposes them as the static library `control_plane_proto` which
# downstream server / aegisctl targets link.
if(NOT ENABLE_CONTROL_PLANE)
    return()
endif()

set(CP_PROTO_IN_DIR  "${CMAKE_SOURCE_DIR}/api/control-plane/proto")
set(CP_PROTO_FILE    "${CP_PROTO_IN_DIR}/control_plane/v1/control_plane.proto")
set(CP_PROTO_OUT_DIR "${CMAKE_BINARY_DIR}/generated")
set(CP_GEN_SUBDIR    "${CP_PROTO_OUT_DIR}/control_plane/v1")

set(CP_PROTO_SRCS
    "${CP_GEN_SUBDIR}/control_plane.pb.cc"
    "${CP_GEN_SUBDIR}/control_plane.grpc.pb.cc"
)
set(CP_PROTO_HDRS
    "${CP_GEN_SUBDIR}/control_plane.pb.h"
    "${CP_GEN_SUBDIR}/control_plane.grpc.pb.h"
)

file(MAKE_DIRECTORY "${CP_GEN_SUBDIR}")

# Resolve protoc + grpc_cpp_plugin locations. vcpkg's gRPC config exports
# imported executable targets; fall back to `find_program` for other toolchains.
if(TARGET protobuf::protoc)
    set(CP_PROTOC "$<TARGET_FILE:protobuf::protoc>")
else()
    find_program(CP_PROTOC protoc REQUIRED)
endif()

if(TARGET gRPC::grpc_cpp_plugin)
    set(CP_GRPC_CPP_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
else()
    find_program(CP_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

add_custom_command(
    OUTPUT  ${CP_PROTO_SRCS} ${CP_PROTO_HDRS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CP_GEN_SUBDIR}"
    COMMAND ${CP_PROTOC}
            --cpp_out "${CP_PROTO_OUT_DIR}"
            --grpc_out "${CP_PROTO_OUT_DIR}"
            --plugin=protoc-gen-grpc=${CP_GRPC_CPP_PLUGIN}
            -I "${CP_PROTO_IN_DIR}"
            "${CP_PROTO_FILE}"
    DEPENDS "${CP_PROTO_FILE}"
    COMMENT "protoc + grpc_cpp_plugin: control_plane.proto"
    VERBATIM
)

add_library(control_plane_proto STATIC ${CP_PROTO_SRCS})
target_include_directories(control_plane_proto
    PUBLIC "${CP_PROTO_OUT_DIR}"
)
target_link_libraries(control_plane_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)
# Generated code triggers a handful of "unused parameter" style diagnostics
# on some toolchains; keep the rest of the project strict.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(control_plane_proto
        PRIVATE -Wno-unused-parameter -Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------------------
# Epic 5 — gRPC adapter + server bootstrap + auth interceptor.
#
# Lives here (not in aegisgate_core) because every translation unit pulls in
# the generated proto headers; OFF-path builds must remain gRPC-free.
# ---------------------------------------------------------------------------

add_library(control_plane_server STATIC
    grpc/config_service_grpc_adapter.cpp
    grpc/rollout_service_grpc_adapter.cpp
    grpc/auth_interceptor.cpp
    grpc/server_bootstrap.cpp
)
target_include_directories(control_plane_server PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(control_plane_server PUBLIC
    control_plane_proto
    aegisgate_core
    gRPC::grpc++
    OpenSSL::Crypto
)

# ---------------------------------------------------------------------------
# Epic 6 — aegisgate-control-plane binary (main.cpp).
#
# This is the *only* long-running executable for the control plane; the
# data-plane `aegisgate` binary is entirely separate. Kept here rather than
# in src/ so it never accidentally leaks into the OFF build.
# ---------------------------------------------------------------------------

add_executable(aegisgate-control-plane main.cpp)
target_link_libraries(aegisgate-control-plane
    PRIVATE
        control_plane_server
        aegisgate_core
        spdlog::spdlog
)

# ---------------------------------------------------------------------------
# Epic 7 — aegisctl client-side helpers (grpc_client, config_cli).
#
# Packaged as a static library so the aegisctl executable can link it in
# only when ENABLE_CONTROL_PLANE=ON, and the unit tests can pull the same
# TU set without re-describing sources. PUBLIC include dir so test
# translation units can `#include "cli/grpc_client.h"` directly.
# ---------------------------------------------------------------------------

add_library(aegisctl_controlplane STATIC
    ${CMAKE_SOURCE_DIR}/src/cli/grpc_client.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/config_cli.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/rollout_cli.cpp
)
target_include_directories(aegisctl_controlplane PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(aegisctl_controlplane PUBLIC
    control_plane_proto
    aegisgate_core
    gRPC::grpc++
)
