# OTEL 离线依赖说明

`protobuf-3.21.12.tar.gz` 已放在项目根目录。为完成 OTEL 构建，还需以下内容。

## opentelemetry-cpp[otlp-http] 依赖链

| 包 | 用途 | 是否已有 |
|----|------|---------|
| **protobuf** 3.21.12 | OTLP 序列化 | ✅ 有 `protobuf-3.21.12.tar.gz` |
| **curl** | OTLP HTTP 导出 | ❌ 需下载 |
| **opentelemetry-cpp** | 追踪 SDK | ❌ 需下载 |
| abseil, nlohmann-json | 基础依赖 | ✅ vcpkg 通常已有 |

## 使用本地 protobuf

vcpkg 会从 GitHub 拉取 `https://github.com/protocolbuffers/protobuf/archive/v3.21.12.tar.gz`。

**将本地文件提供给 vcpkg：**

```bash
cp /path/to/loong-aegisgate/protobuf-3.21.12.tar.gz \
   /path/to/vcpkg/downloads/protocolbuffers-protobuf-v3.21.12.tar.gz
```

vcpkg 期望文件名为 `protocolbuffers-protobuf-v3.21.12.tar.gz`，且 SHA512 须为：
```
152f8441c325e808b942153c15e82fdb533d5273b50c25c28916ec568ada880f79242bb61ee332ac5fb0d20f21239ed6f8de02ef6256cc574b1fc354d002c6b0
```

若 GitHub 官方 tarball 与上述 SHA512 不一致，需改用正确来源的文件。

## 需要额外下载的内容

即使已有 protobuf，仍需要：

1. **curl** — opentelemetry-cpp[otlp-http] 的依赖  
2. **opentelemetry-cpp** — 主包本身（以及 curl、protobuf 未命中缓存时）

在有网络的环境下，执行（注意 `VCPKG_MANIFEST_FEATURES` 为 CMake 变量，非环境变量）：
```bash
cmake -B build-otel -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DENABLE_OPENTELEMETRY=ON \
  -DVCPKG_MANIFEST_FEATURES=otel
```
vcpkg 会自动拉取并构建上述依赖。

使用代理时：`export http_proxy=http://HOST:PORT https_proxy=http://HOST:PORT`

## 完全离线的方式

1. 在网络环境执行 `vcpkg install opentelemetry-cpp[otlp-http]`，安装完成后打包 `vcpkg_installed/`（或 `~/.cache/vcpkg/archives/`）到离线环境；
2. 或将依赖源码预先下载到 `VCPKG_DOWNLOADS` 目录，并按 vcpkg 的命名规则放置（需查阅 vcpkg 源码中对应 port 的文件命名）。
