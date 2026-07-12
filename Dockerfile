# syntax=docker/dockerfile:1

# Stage 1: toolchain + pinned vcpkg + Release build (root; only artifacts propagate)
FROM ubuntu:22.04 AS builder

# Pin vcpkg ref (tag, branch, or commit). Override at build: docker build --build-arg VCPKG_TAG=...
ARG VCPKG_TAG=2025.10.17

RUN apt-get update && apt-get install -y \
    build-essential cmake git curl zip unzip tar pkg-config \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && cd /opt/vcpkg \
    && git checkout "${VCPKG_TAG}" \
    && ./bootstrap-vcpkg.sh

WORKDIR /app
COPY vcpkg.json .
COPY CMakeLists.txt .
COPY cmake/ cmake/
COPY src/ src/
COPY include/ include/
COPY tests/ tests/
COPY config/ config/

# 容器化构建档位 ARGs（与 scripts/build.sh 档位概念对齐 / TASK-20260618-01 P1-C）:
#   精简档位（默认 / 与 scripts/build.sh -t Debug 一致）：四 ARG 全 OFF / 适合本地体验、CI。
#   生产档位（与 scripts/build.sh -t Release 全开等价）：
#     docker build \
#       --build-arg ENABLE_REDIS=ON \
#       --build-arg ENABLE_PG=ON \
#       --build-arg ENABLE_OPENTELEMETRY=ON \
#       --build-arg ENABLE_CONTROL_PLANE=ON \
#       --build-arg VCPKG_FEATURES="guard;guard-spm;redis;pg;otel;control-plane" \
#       -t aegisgate:prod .
#   生产档位必须四 ARG 全 ON 同时 VCPKG_FEATURES 覆盖对应 feature，否则
#   find_package 在 cmake 阶段就会失败（hiredis / libpq / grpc / opentelemetry-cpp）；
#   缺 `guard`（onnxruntime）时 ENABLE_GUARD_MODEL 会 graceful 降为 OFF。
#
# 历史教训（commit 3542b0c）：生产档位才暴露的链接缺陷不能只在精简档位下验证；
# 见 memory-bank/systemPatterns.md「验证必须在『生产档位』全开下进行」。
ARG ENABLE_REDIS=OFF
ARG ENABLE_PG=OFF
ARG ENABLE_OPENTELEMETRY=OFF
ARG ENABLE_CONTROL_PLANE=OFF
ARG VCPKG_FEATURES=""

RUN cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DENABLE_REDIS=${ENABLE_REDIS} \
    -DENABLE_PG=${ENABLE_PG} \
    -DENABLE_OPENTELEMETRY=${ENABLE_OPENTELEMETRY} \
    -DENABLE_CONTROL_PLANE=${ENABLE_CONTROL_PLANE} \
    -DVCPKG_MANIFEST_FEATURES="${VCPKG_FEATURES}" \
    && cmake --build build -j"$(nproc)"

# Stage 2: minimal runtime — no compiler/vcpkg; non-root + explicit health probe deps
FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y \
    libstdc++6 ca-certificates wget \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system aegisgate \
    && useradd --system --gid aegisgate --no-create-home --home-dir /app aegisgate

WORKDIR /app

COPY --from=builder --chown=aegisgate:aegisgate /app/build/src/aegisgate .
COPY --chown=aegisgate:aegisgate config/ config/

# TASK-20260525-01 MVP-1: ship quickstart entrypoint + minimal config.
# Does NOT change default ENTRYPOINT/CMD below (zero impact on existing users).
# Users opt into quickstart mode via:
#   docker run --entrypoint /usr/local/bin/quickstart-entrypoint.sh aegisgate:latest
COPY --chown=root:root --chmod=755 scripts/quickstart-entrypoint.sh /usr/local/bin/

RUN chown aegisgate:aegisgate /app

USER aegisgate

EXPOSE 8080 9090

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD wget --no-verbose --tries=1 --spider http://127.0.0.1:8080/health/ready || exit 1

ENTRYPOINT ["./aegisgate"]
CMD ["config/aegisgate.yaml"]
