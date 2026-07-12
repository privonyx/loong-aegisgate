# OTEL Offline Dependencies

`protobuf-3.21.12.tar.gz` is placed at the project root. The following are still required to complete an OTEL build.

## opentelemetry-cpp[otlp-http] dependency chain

| Package | Purpose | Already available |
|---------|---------|-------------------|
| **protobuf** 3.21.12 | OTLP serialization | Yes — `protobuf-3.21.12.tar.gz` |
| **curl** | OTLP HTTP export | No — must download |
| **opentelemetry-cpp** | Tracing SDK | No — must download |
| abseil, nlohmann-json | Base dependencies | Usually present via vcpkg |

## Using local protobuf

vcpkg fetches `https://github.com/protocolbuffers/protobuf/archive/v3.21.12.tar.gz` from GitHub.

**Provide the local file to vcpkg:**

```bash
cp /path/to/loong-aegisgate/protobuf-3.21.12.tar.gz \
   /path/to/vcpkg/downloads/protocolbuffers-protobuf-v3.21.12.tar.gz
```

vcpkg expects the filename `protocolbuffers-protobuf-v3.21.12.tar.gz`, and the SHA512 must be:

```
152f8441c325e808b942153c15e82fdb533d5273b50c25c28916ec568ada880f79242bb61ee332ac5fb0d20f21239ed6f8de02ef6256cc574b1fc354d002c6b0
```

If the official GitHub tarball does not match this SHA512, use a tarball from a source that produces the correct hash.

## What you still need to download

Even with protobuf in place, you still need:

1. **curl** — dependency of opentelemetry-cpp[otlp-http]  
2. **opentelemetry-cpp** — the main port (and curl, protobuf when not served from cache)

On a machine with network access, run (note: `VCPKG_MANIFEST_FEATURES` is a CMake variable, not an environment variable):

```bash
cmake -B build-otel -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DENABLE_OPENTELEMETRY=ON \
  -DVCPKG_MANIFEST_FEATURES=otel
```

vcpkg will fetch and build the dependencies above.

With a proxy: `export http_proxy=http://HOST:PORT https_proxy=http://HOST:PORT`

## Fully offline workflow

1. On a networked machine, run `vcpkg install opentelemetry-cpp[otlp-http]`, then archive `vcpkg_installed/` (or `~/.cache/vcpkg/archives/`) and copy it to the offline environment; or  
2. Pre-download dependency sources into `VCPKG_DOWNLOADS` and name them per vcpkg’s conventions (see the relevant port in the vcpkg tree for expected filenames).
