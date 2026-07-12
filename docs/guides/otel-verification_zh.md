# OpenTelemetry 构建验证指南

在合并 Phase 1.1 可观测性分支前，需验证 `ENABLE_OPENTELEMETRY=ON` 构建和测试通过。

## 前置条件

- 网络可用（vcpkg 需拉取 opentelemetry-cpp）
- vcpkg 已配置（使用项目 vcpkg.json）

## 步骤

1. **配置构建（需传递 `-DVCPKG_MANIFEST_FEATURES=otel` 以启用 otel feature）：**

   ```bash
   cmake -B build-otel -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
     -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DENABLE_OPENTELEMETRY=ON \
     -DVCPKG_MANIFEST_FEATURES=otel
   ```

2. **构建与测试：**

   ```bash
   ninja -C build-otel
   ninja -C build-otel test
   ```

3. **已知问题**：`TracingOtelTest` 的 5 个用例因 BatchSpanProcessor 已知问题（opentelemetry-cpp #3071, #3394）已设为 DISABLED，默认不运行。可手动执行：`./build-otel/tests/unit/observe/test_tracing --gtest_also_run_disabled_tests --gtest_filter=*TracingOtel*`

4. **通过后合并** `feature/TASK-20260321-08-phase1.1-observability` 分支。
