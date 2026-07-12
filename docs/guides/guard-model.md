# Guard Model Deployment

`GuardClassifier` can run an optional ONNX prompt-injection classifier when
`ENABLE_GUARD_MODEL=ON` and `security.guard_model.enabled=true`.

## Build

`ENABLE_GUARD_MODEL` is **default-ON** (a base capability since TASK-20260614-01).
It depends on both the ONNX Runtime and the SentencePiece tokenizer:

- **ONNX Runtime** resolves from either:
  - the local `third_party/onnxruntime-<os>-<arch>-<ver>` package
    (`scripts/fetch-onnxruntime.sh`), or
  - the `guard` vcpkg feature (`onnxruntime`).
- **SentencePiece** is provided by the `guard-spm` vcpkg feature, which is **not**
  installed in standard builds. To fully enable the guard path, reconfigure
  with:

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_FEATURES="guard;guard-spm"
```

If either dependency is missing, the build **gracefully downgrades**
`ENABLE_GUARD_MODEL` to OFF with a CMake warning (it does not fail configure), and
the safety guard stays inactive until you install the dependency and reconfigure.

## Model

The current v1 model is:

- HuggingFace repo: `protectai/deberta-v3-base-prompt-injection-v2`
- License: Apache-2.0
- ONNX path: `onnx/model.onnx`
- Tokenizer: SentencePiece (`onnx/spm.model`)
- Expected labels: `safe`, `injection`

Download the model artifacts:

```bash
scripts/download_guard_model.sh
```

By default this writes to `models/guard/`, which is ignored by git because the
ONNX file is large. If your network requires a mirror or proxy:

```bash
HF_ENDPOINT=https://huggingface.co scripts/download_guard_model.sh
CURL_OPTS="--proxy http://127.0.0.1:7890" scripts/download_guard_model.sh
```

## Configuration

```yaml
security:
  guard_model:
    enabled: true
    model_path: "models/guard/deberta-v3-base-prompt-injection-v2.onnx"
    spm_model_path: "models/guard/deberta-v3-base-prompt-injection-v2.spm.model"
    threshold: 0.5
    fail_policy: open
```

`fail_policy` controls model/tokenizer failure behavior:

- `open`: allow the request if the local guard model is unavailable.
- `closed`: reject the request and audit `guard_fail_closed`.

Use `closed` only when your deployment treats the local model as a hard security
dependency and has operational monitoring for model availability.

## Tokenizer Notes

DeBERTa-v3 uses SentencePiece rather than WordPiece. Do not use
`models/vocab.txt` or `BertTokenizer` for this model: token IDs will not match
the ONNX export and prompt-injection detection quality will drift.

The wrapper adds model special tokens as `[CLS]=1`, `[SEP]=2`, `[PAD]=0`, then
emits two tensors: `input_ids` and `attention_mask`.

## Limits

This guard is one signal in the inbound pipeline, not a complete policy engine.
Keep `InjectionDetector`, `TopicGuard`, audit logging, rate limits, and external
safety providers configured according to your risk profile.
