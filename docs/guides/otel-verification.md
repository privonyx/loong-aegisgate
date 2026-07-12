# OpenTelemetry Build Verification Guide

Before merging the Phase 1.1 observability branch, verify that the build and tests pass with `ENABLE_OPENTELEMETRY=ON`.

## Prerequisites

- Network available (vcpkg must fetch opentelemetry-cpp)
- vcpkg configured (using the project `vcpkg.json`)

## Steps

1. **Configure the build (pass `-DVCPKG_MANIFEST_FEATURES=otel` to enable the otel feature):**

   ```bash
   cmake -B build-otel -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
     -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DENABLE_OPENTELEMETRY=ON \
     -DVCPKG_MANIFEST_FEATURES=otel
   ```

2. **Build and test:**

   ```bash
   ninja -C build-otel
   ninja -C build-otel test
   ```

3. **Known issue:** Five cases in `TracingOtelTest` are marked DISABLED due to known BatchSpanProcessor issues (opentelemetry-cpp #3071, #3394) and do not run by default. To run them manually: `./build-otel/tests/unit/observe/test_tracing --gtest_also_run_disabled_tests --gtest_filter=*TracingOtel*`

4. **After passing**, merge the `feature/TASK-20260321-08-phase1.1-observability` branch.
