# AegisGate Provider Manifest Specification

> **apiVersion**: `aegisgate.dev/v1alpha1`
> **Kind**: `ProviderManifest`
> **Status**: Alpha — interface may evolve; breaking changes will bump `apiVersion`.
> **Introduced in**: Phase 10.1 / v3.0 roadmap

A **Provider Manifest** declares the *contract* between a model provider and AegisGate in a machine-readable YAML document. It is distinct from `config/models.yaml` (the *runtime instance* configuration). The Manifest:

1. documents a provider's metadata and capabilities,
2. formally describes its OpenAI chat/completions compatibility,
3. enables tooling (CI / IDE / `aegisctl conformance`) to validate a provider before it is deployed, and
4. gives third-party providers a stable contract to target without forking AegisGate source.

Every official AegisGate Provider has a Manifest under `spec/providers/<name>.yaml`; third-party providers are encouraged to publish one alongside their SDK or plugin.

## 1. Document Structure

```yaml
apiVersion: aegisgate.dev/v1alpha1
kind: ProviderManifest
metadata:    # identity and discovery
  ...
spec:        # contract
  connector:
    ...
  endpoint:
    ...
  auth:
    ...
  compatibility:
    ...
  capabilities:
    ...
  models:
    ...
  conformance:
    ...
```

## 2. `apiVersion` & `kind`

| Field | Required | Value |
|-------|:--------:|-------|
| `apiVersion` | ✓ | Exactly `aegisgate.dev/v1alpha1` at this stage |
| `kind` | ✓ | Exactly `ProviderManifest` |

Any other value is an **Error** during validation.

## 3. `metadata`

| Field | Required | Constraint |
|-------|:--------:|-----------|
| `name` | ✓ | `[a-z0-9_-]{1,64}`; globally unique; also used as `ConnectorFactory` type |
| `display_name` | — | Human-readable name |
| `vendor` | — | Legal entity responsible for the provider |
| `homepage` | — | URL |
| `documentation` | — | URL to provider API docs |
| `tags` | — | Array of short labels (`commercial`, `chat`, `tools`, `vision`, ...) |
| `maturity` | — | `experimental` \| `preview` (default) \| `stable` |

## 4. `spec.connector`

| Field | Required | Value |
|-------|:--------:|-------|
| `kind` | ✓ | `openai` \| `claude` \| `external` |

`openai` and `claude` map to the existing built-in connectors. `external` is reserved for Phase 10.1.3 (gRPC sidecar provider protocol). Any other value is an **Error**.

## 5. `spec.endpoint`

| Field | Required | Constraint |
|-------|:--------:|-----------|
| `base_url_default` | ✓ | Starts with `http://` or `https://` |
| `base_url_env` | — | Name of an environment variable that can override `base_url_default` |
| `timeout_ms_default` | — | Default: `30000` |
| `max_retries_default` | — | Default: `2` |

## 6. `spec.auth`

| Field | Required | Value / Constraint |
|-------|:--------:|-------------------|
| `type` | ✓ | `bearer` \| `api_key_header` \| `query` \| `none` |
| `header_name` | — | e.g. `Authorization`, `X-API-Key` |
| `env_var` | — | Default env var for the API key (`OPENAI_API_KEY`, ...) |
| `supports_multi_key` | — | Default: `false` |

**Conformance rule** (warning): if `type` is `bearer` or `api_key_header` **and** both `header_name` and `env_var` are empty, a warning `conformance.auth-defined` is emitted — callers cannot authenticate unambiguously.

## 7. `spec.compatibility`

| Field | Required | Value |
|-------|:--------:|-------|
| `openai_chat_completions` | ✓ | `full` \| `partial` \| `translated` \| `none` |
| `fields[]` | — | Per-field compatibility declarations |

Each `fields[]` entry:

| Field | Required | Value |
|-------|:--------:|-------|
| `name` | ✓ | OpenAI chat/completions request field name |
| `status` | ✓ | `supported` \| `unsupported` \| `translated` \| `ignored` |
| `notes` | — | Free-form |

**Conformance rule** (warning): declaring a field that is **not** one of the known OpenAI chat/completions fields emits `conformance.fields-coverage`.

### 7.1 `openai_chat_completions` Levels

| Level | Meaning |
|-------|---------|
| `full` | Every OpenAI request field is accepted; response is bit-compatible |
| `partial` | Most fields work; a few are unsupported (documented in `fields[]`) |
| `translated` | AegisGate translates OpenAI-shape requests to a different native format (e.g. Claude). Behavior should still be OpenAI-like to clients |
| `none` | Not an OpenAI-compatible endpoint (e.g. a proprietary connector) |

**Conformance rule** (warning): `connector.kind=claude` with `openai_chat_completions=full` is unusual (Claude natively uses a different request shape); expected level is `translated`.

## 8. `spec.capabilities[]`

String list drawn from the following enum (matches `Capability` in C++):

```
streaming | tools | vision | response_format | logprobs |
system_message | temperature | top_p | max_tokens
```

**Conformance rule** (error): unknown entries emit `conformance.capability-enum`.

## 9. `spec.models[]`

Optional contract-level declaration of which model IDs this provider serves.

| Field | Required | Constraint |
|-------|:--------:|-----------|
| `id` | ✓ | Unique within the Manifest |
| `max_context_tokens` | — | Integer |
| `capabilities` | — | Subset of `spec.capabilities` |
| `region_hints` | — | Array of region strings; useful for `GeoRouter` model tagging |

## 10. `spec.conformance`

Optional. Encodes static conformance expectations — no live HTTP calls are performed.

| Field | Required | Constraint |
|-------|:--------:|-----------|
| `required_checks[]` | — | Names of checks the provider claims to pass |
| `sample_request` | — | JSON shape for a sanity request; must include `model` and a non-empty `messages[]` if declared |
| `sample_response_shape` | — | JSON shape the runner expects the provider's response to satisfy |

**Conformance rules**:

- `conformance.sample-request-shape` (error): `sample_request` without `messages[]` or `model`
- `conformance.models-unique` (error): duplicate model ids inside `spec.models[]`

## 11. Validation vs Conformance

| Layer | What it catches | Output |
|-------|-----------------|--------|
| **Structural validation** (`validateManifest`) | Missing required fields, enum violations, illegal names, bad URLs | Mostly **Errors** |
| **Conformance** (`runConformanceChecks`) | Ambiguous auth, suspect compat claims, unknown field declarations, unknown capabilities, malformed sample requests | Mix of Warnings and Errors |

Both return a `ValidationReport` and are composable. The convention:

- **Error** — blocks acceptance (runner returns non-zero exit)
- **Warning** — surfaces in logs / CLI output, but does not block

## 12. Tooling

`aegisctl conformance check <file>` runs both stages on a single Manifest.
`aegisctl conformance check-all <dir>` runs them on every `*.yaml` in a directory.

Machine-readable output is available in the C++ API (`ValidationReport.issues[]`) — each issue carries a stable `code` (e.g. `spec.auth.type.unknown`) suitable for CI assertions.

## 13. Versioning & Evolution

- `apiVersion` increments when incompatible schema changes are needed.
- Additive changes (new optional fields) are allowed without a version bump.
- Deprecated fields remain in the parser for one `apiVersion` cycle before removal.

## 14. Roadmap

| Phase | Item |
|-------|------|
| ✅ 10.1.1 | Provider Manifest schema (this document) |
| ✅ 10.1.2 | JSON Schema + Conformance v0 |
| 10.1.3 | `external` connector — gRPC sidecar protocol |
| 10.1.4 | Runtime cross-check: startup compares `spec/providers/*.yaml` to `config/models.yaml` |
| 10.2 | WASM-based plugins consume Manifest for capability negotiation |
| 10.3 | Rule Hub publishes Manifest alongside provider artifacts |

## 15. References

- JSON Schema: `schema/provider.schema.json`
- Example manifests: `spec/providers/*.yaml`
- OpenAI compatibility matrix: `docs/openai-compat-matrix.md`
- C++ API: `src/gateway/provider_spec/provider_manifest.h`
- Spec design doc: `docs/specs/2026-04-17-phase10.1-provider-manifest-design.md`
