# OpenAI Chat/Completions Compatibility Matrix

> **Source of truth:** `api/providers/definitions/*.yaml` Manifests
> **Last refresh:** 2026-04-17 (Phase 10.1)
> **Refresh policy:** any change to a Manifest's `compatibility.fields[]` requires updating this table

This document compares each official AegisGate Provider's compatibility with the OpenAI `/v1/chat/completions` request shape. Use it to:

- pick a provider that supports the request fields you need;
- understand which fields are silently translated (e.g. for Anthropic Claude);
- decide whether to enable certain features per-tenant.

## Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | `supported` — passes through to provider as-is |
| 🔁 | `translated` — AegisGate rewrites the field for native API (e.g. Claude) |
| ⚠️ | `partial` — supported with caveats, see notes |
| ❌ | `unsupported` — provider rejects or AegisGate strips |
| `—` | not declared in Manifest (assumed supported per `openai_chat_completions` level) |

## Compatibility level (top of provider Manifest)

| Provider | `openai_chat_completions` | Connector kind | Maturity |
|----------|:-------------------------:|:--------------:|:--------:|
| openai | full | openai | stable |
| claude | translated | claude | stable |
| deepseek | full | openai | stable |
| doubao | partial | openai | preview |
| qwen | full | openai | stable |
| gemini | full | openai | preview |
| mistral | full | openai | stable |

## Field-level support

| Field | openai | claude | deepseek | doubao | qwen | gemini | mistral |
|-------|:------:|:------:|:--------:|:------:|:----:|:------:|:-------:|
| `messages` | ✅ | 🔁 | ✅ | ✅ | ✅ | ✅ | ✅ |
| `model` | ✅ | — | — | — | — | — | — |
| `temperature` | ✅ | — | — | — | — | — | — |
| `top_p` | ✅ | — | — | — | — | — | — |
| `max_tokens` | ✅ | — | — | — | — | — | — |
| `stream` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `stop` | ✅ | — | — | — | — | — | — |
| `tools` | ✅ | 🔁 | ✅ | ❌ | ✅ | ✅ | ✅ |
| `tool_choice` | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ |
| `response_format` | ✅ | ❌ | ✅ | ❌ | ⚠️ | ✅ | ✅ |
| `logprobs` | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| `presence_penalty` | — | — | — | — | — | — | — |
| `frequency_penalty` | — | — | — | — | — | — | — |
| `seed` | — | — | — | — | — | — | — |
| `n` | — | — | — | — | — | — | — |

### Notes

- **claude**: `messages` undergoes OpenAI ↔ Anthropic translation by `ClaudeConnector` (handled in `parseStreamChunk` and `buildRequestBody`); `tools` round-trips between OpenAI tool_calls and Anthropic tool_use/tool_result blocks.
- **doubao**: tool calling support varies by deployed endpoint id — declared `unsupported` to be conservative.
- **qwen**: `response_format=json_object` works; `json_schema` is partially supported by upstream.
- **logprobs** is universally unsupported — OpenAI exposes it only on the legacy `/v1/completions` endpoint, and AegisGate currently does not bridge it.

## Capabilities cross-reference

This is `spec.capabilities[]` from each Manifest:

| Capability | openai | claude | deepseek | doubao | qwen | gemini | mistral |
|------------|:------:|:------:|:--------:|:------:|:----:|:------:|:-------:|
| streaming | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| tools | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ |
| vision | ✅ | ✅ | — | — | ✅ | ✅ | — |
| response_format | ✅ | — | ✅ | — | ✅ | ✅ | ✅ |
| system_message | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| temperature | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| top_p | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| max_tokens | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| logprobs | — | — | — | — | — | — | — |

## How to verify

```bash
aegisctl conformance check api/providers/definitions/openai.yaml
aegisctl conformance check-all api/providers/definitions/
```

CI runs the same checks on every PR that touches `api/providers/definitions/`.

## Updating this matrix

1. Modify the corresponding `api/providers/definitions/<name>.yaml`
2. Run `aegisctl conformance check api/providers/definitions/<name>.yaml` — fix any reported issues
3. Update the corresponding row in the table above
4. Submit a single PR containing the Manifest and the matrix update together
