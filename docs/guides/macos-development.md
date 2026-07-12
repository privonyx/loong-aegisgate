# macOS（Apple Silicon）开发指南

本指南介绍如何在 macOS（Apple Silicon / `arm64-osx`）上原生编译与开发 AegisGate。

> 适用范围：仅 Apple Silicon（M 系列，`arm64-osx`）原生编译。Intel `x64-osx` 未在本轮支持范围内。

## 1. 前置依赖

通过 [Homebrew](https://brew.sh) 安装工具链：

```bash
# Xcode 命令行工具（提供 Apple Clang）
xcode-select --install

# 构建工具
brew install cmake ninja pkg-config

# 前端 / SDK（按需）
brew install node go
```

## 2. 获取并 bootstrap vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg"
```

> `scripts/build.sh` 已支持在 macOS 上从 `$HOME` 与 Homebrew 路径（`/opt/homebrew`）自动探测 vcpkg；
> 也可显式 `export VCPKG_ROOT=...`。

## 3. 获取 ONNX Runtime（guard / onnx feature 需要）

CMake 按决策 A3 解析 ONNX Runtime（详见可行性报告 §3）。在 macOS 上用脚本拉取官方预编译包：

```bash
# 按平台下载 onnxruntime-osx-arm64-<ver> 到 third_party/（含 sha256 校验）
scripts/fetch-onnxruntime.sh
```

> **供应链安全（fail-closed）：** 脚本默认要求 sha256 pin。首次在 macOS 下载时，
> 若脚本内尚无 `osx-arm64` 的 pin，会拒绝并提示。两种放行方式：
> - 已知期望值：`ONNXRUNTIME_SHA256=<hex> scripts/fetch-onnxruntime.sh`
> - 首次取值：`scripts/fetch-onnxruntime.sh --allow-unverified`，记录打印的 sha256 后
>   回填到脚本 `SHA256_PINS[osx-arm64-1.24.2]` 以固化。
>
> 备选：不下载预编译包时，CMake 会回退到 vcpkg `find_package(onnxruntime CONFIG)`
> （注意 vcpkg 端口版本为 1.23.2，且为源码编译，耗时较长）。

## 4. 构建

一键开发环境：

```bash
./scripts/setup-dev.sh
```

或手动配置（全 feature 示例）：

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DENABLE_GUARD_MODEL=ON \
  -DENABLE_PG=ON -DVCPKG_MANIFEST_FEATURES="pg" \
  # 其余 feature 按需: -DENABLE_REDIS=ON / -DENABLE_OPENTELEMETRY=ON /
  #                    -DENABLE_CONTROL_PLANE=ON 并加对应 VCPKG_MANIFEST_FEATURES

cmake --build build -j"$(sysctl -n hw.ncpu)"
```

> feature 开关与对应的 `VCPKG_MANIFEST_FEATURES`（见 `vcpkg.json`）：
> `guard`(onnxruntime) / `pg`(libpq) / `redis`(hiredis) / `otel`(opentelemetry-cpp) /
> `control-plane`(grpc+protobuf) / `icu`。

## 5. 测试

```bash
cd build && ctest --output-on-failure
```

脚本自检（跨平台助手）：

```bash
bash tests/scripts/test-platform-helper.sh
bash tests/rules/test-task20260607-01-macos-portability.sh
```

## 6. 插件

原生插件共享库扩展名随平台变化：**Linux `.so` / macOS `.dylib`**。
在 `config/aegisgate.yaml` 的 `plugins.stages[].path` 中使用对应平台后缀。

## 7. 已知坑

- **vcpkg `arm64-osx`**：首次安装全 feature 依赖耗时较长（grpc/protobuf/onnxruntime 等）。建议预留时间并开启 vcpkg 二进制缓存。
- **Apple Clang + libc++**：与 Linux GCC + libstdc++ 在 `-Werror` 下的告警集合可能不同。若首次编译出现新告警，请逐项评估（项目已清除 OIDC 的 OpenSSL 弃用告警）。
- **`test-control-plane-local.sh`**：当前仍使用 Linux 专有的 `ss` 做端口监听探测，在 macOS 上需改用 `lsof`/`netstat`（尚未改造，见可行性报告 backlog）。
- **Docker**：macOS 上 Docker 经虚拟机运行 Linux 容器；项目镜像目前仅 `linux/amd64`，在 Apple Silicon 上需仿真，性能下降，建议本地开发优先用原生编译而非容器。

## 8. 故障排查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| `find_package(onnxruntime)` 失败 | 未获取 macOS 预编译包 | 运行 `scripts/fetch-onnxruntime.sh` |
| `fetch-onnxruntime.sh` 报 fail-closed | 无 sha256 pin | 提供 `ONNXRUNTIME_SHA256` 或 `--allow-unverified` 后回填 pin |
| 脚本 `grep -P` / `sed -i` 报错 | 使用了未改造的旧脚本 | 确认脚本已 `source scripts/lib/platform.sh` |
| vcpkg 找不到 | `VCPKG_ROOT` 未设 | `export VCPKG_ROOT=$HOME/vcpkg` |
