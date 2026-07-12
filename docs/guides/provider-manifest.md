# Provider Manifest Guide

> Feature: Provider Manifest + Conformance Suite (Phase 10.1)
> Available: v1.1+ (delivered incrementally under the v3.0 Phase 10 roadmap)

This guide shows how to:

1. Read an official AegisGate Provider Manifest
2. Write a Manifest for a new / third-party Provider
3. Validate a Manifest with `aegisctl conformance`
4. Use the Manifest as a contract in CI

For the full schema, see [`api/providers/manifest.md`](../../api/providers/manifest.md) and [`api/providers/provider.schema.json`](../../api/providers/provider.schema.json).

## Why a Manifest?

Previously, adding a new provider to AegisGate meant forking the C++ source:

```text
src/gateway/connector/myprovider.{h,cpp}  # implement ModelConnector
src/gateway/connector/factory.cpp         # registerType(...)
config/models.yaml                        # add YAML entry
```

With a **Provider Manifest**, the *contract* (identity, endpoint, auth, OpenAI compatibility, capabilities) lives in a YAML document that can be:

- validated by `aegisctl conformance` before merging
- consumed by IDEs / editors for autocomplete
- shipped alongside a third-party plugin/SDK without forking AegisGate

Manifest is **orthogonal** to the runtime configuration `config/models.yaml`:

| | Manifest (`api/providers/definitions/*.yaml`) | Runtime (`config/models.yaml`) |
|---|---|---|
| Role | Contract — "what this provider is" | Instance — "how to run it here" |
| Lifecycle | Published once with the provider | Edited per deployment |
| Read by | CI, IDE, `aegisctl conformance` | Gateway runtime, `ConnectorFactory` |

## 1. Reading an official Manifest

Start with `api/providers/definitions/openai.yaml`:

```yaml
apiVersion: aegisgate.dev/v1alpha1
kind: ProviderManifest
metadata:
  name: openai
  display_name: OpenAI
  maturity: stable
spec:
  connector:
    kind: openai
  endpoint:
    base_url_default: https://api.openai.com/v1
  auth:
    type: bearer
    env_var: OPENAI_API_KEY
  compatibility:
    openai_chat_completions: full
  capabilities:
    - streaming
    - tools
    - vision
  # models / conformance are optional extensions
```

The essential fields:

- `metadata.name` — also the `ConnectorFactory` type string used in `config/models.yaml`
- `spec.connector.kind` — which built-in connector runs it (`openai`, `claude`, or future `external`)
- `spec.compatibility.openai_chat_completions` — the compatibility level, one of `full`/`partial`/`translated`/`none`
- `spec.capabilities` — the capability enum declared by the provider

## 2. Writing a Manifest for a new provider

### Step 1: start from a template

Copy one of the 7 official manifests as the template:

```bash
cp api/providers/definitions/openai.yaml api/providers/definitions/my-provider.yaml
```

### Step 2: fill out `metadata`

```yaml
metadata:
  name: my-provider                 # [a-z0-9_-]{1,64}
  display_name: My LLM Provider
  vendor: ACME Corp
  homepage: https://example.com
  documentation: https://example.com/docs
  tags: [commercial, chat]
  maturity: preview                  # experimental | preview | stable
```

### Step 3: declare the endpoint & auth

```yaml
spec:
  connector:
    kind: openai                     # reuse the OpenAI connector if your API is OpenAI-compatible
  endpoint:
    base_url_default: https://api.example.com/v1
    base_url_env: MY_PROVIDER_BASE_URL
  auth:
    type: bearer
    header_name: Authorization
    env_var: MY_PROVIDER_API_KEY
    supports_multi_key: true
```

### Step 4: declare OpenAI compatibility

```yaml
  compatibility:
    openai_chat_completions: full    # or partial / translated / none
    fields:
      - name: messages
        status: supported
      - name: tools
        status: supported
      - name: response_format
        status: unsupported
        notes: Not yet supported upstream.
```

### Step 5: declare capabilities

```yaml
  capabilities:
    - streaming
    - tools
    - system_message
    - temperature
    - top_p
    - max_tokens
```

Capabilities must be drawn from the enum:
`streaming | tools | vision | response_format | logprobs | system_message | temperature | top_p | max_tokens`

### Step 6: (optional) declare contracts

```yaml
  models:
    - id: my-model-large
      max_context_tokens: 128000
      capabilities: [streaming, tools]
      region_hints: [us-east]

  conformance:
    required_checks:
      - manifest-shape
      - auth-defined
      - compatibility-declared
      - capability-enum
      - models-unique
      - sample-request-shape
    sample_request:
      model: my-model-large
      messages:
        - role: user
          content: ping
      max_tokens: 16
```

## 3. Validate with `aegisctl conformance`

```bash
# Single manifest
aegisctl conformance check api/providers/definitions/my-provider.yaml

# All manifests in a directory
aegisctl conformance check-all api/providers/definitions/
```

Sample output:

```
PASS  api/providers/definitions/my-provider.yaml  (errors=0, warnings=0)
```

On failure, the CLI prints stable issue codes that CI can assert against:

```
FAIL  api/providers/definitions/my-provider.yaml  (errors=1, warnings=0)
    [E] spec.auth.type.unknown :: Unknown auth.type; expected bearer, api_key_header, query, or none  (spec.auth.type)
```

All issue codes are documented in `api/providers/manifest.md` §11.

## 4. CI integration

```yaml
# .github/workflows/manifest-check.yml
name: Provider Manifest Conformance
on: [push, pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build aegisctl
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target aegisctl
      - name: Run conformance
        run: ./build/aegisctl conformance check-all api/providers/definitions/
```

Any error-level issue causes `aegisctl conformance` to exit non-zero, blocking the merge.

## 5. Using the Manifest from code

```cpp
#include "gateway/provider_spec/provider_manifest.h"

aegisgate::ValidationReport report;
auto manifest = aegisgate::loadManifestFromFile("api/providers/definitions/openai.yaml", report);
if (!manifest || !report.ok()) {
    // handle errors; print report.issues[]
}

auto validation = aegisgate::validateManifest(*manifest);
auto conformance = aegisgate::runConformanceChecks(*manifest);
// both are `ValidationReport`s — .ok() / .errorCount() / .warningCount()
```

## 6. Roadmap

| Phase | Item |
|-------|------|
| ✅ 10.1.1 | Manifest schema + JSON Schema + Conformance v0 |
| 10.1.3 | `external` connector — gRPC sidecar protocol |
| 10.1.4 | Runtime cross-check against `config/models.yaml` |
| 10.2 | WASM plugins consume Manifest for capability negotiation |

## 7. Related documents

- [Provider Manifest Specification](../../api/providers/manifest.md)
- [JSON Schema](../../api/providers/provider.schema.json)
- [OpenAI Compatibility Matrix](../openai-compat-matrix.md)
