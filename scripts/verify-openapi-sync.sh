#!/usr/bin/env bash
# scripts/verify-openapi-sync.sh
#
# Phase 9.3 Epic 8.1 — guard against OpenAPI drift from proto.
#
# Ensures every message-field name in the proto source appears verbatim as a
# schema property in the hand-written OpenAPI yaml. The `protoc-gen-openapiv2`
# generator is not in our vcpkg baseline (01f6021), so we maintain
# openapi/control-plane-v1.yaml by hand and rely on this script + CI to catch
# stale yaml.
#
# Scope and known limitations:
#   * We check PRESENCE of field names only, not type correctness (OpenAPI and
#     protobuf type systems are not 1:1; attempting a strict mapping produces
#     noise).
#   * Proto enum *values* are not field names and are intentionally excluded.
#   * A field name that legitimately exists in multiple messages only needs to
#     appear once in the OpenAPI yaml; we do not verify cardinality.
#
# Usage:
#   scripts/verify-openapi-sync.sh          # uses the default paths
#   OPENAPI_FILE=/tmp/draft.yaml \
#   PROTO_FILE=/tmp/draft.proto \
#     scripts/verify-openapi-sync.sh        # override for tests
#
# Exit codes:
#   0 — every proto field name appears in the OpenAPI yaml
#   1 — one or more proto fields are missing from the yaml (details on stderr)
#   2 — misconfiguration (missing input file)

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

OPENAPI_FILE="${OPENAPI_FILE:-$REPO/api/control-plane/openapi/control-plane-v1.yaml}"
PROTO_FILE="${PROTO_FILE:-$REPO/api/control-plane/proto/control_plane/v1/control_plane.proto}"

if [[ ! -f "$OPENAPI_FILE" ]]; then
    printf 'ERROR: OpenAPI file missing: %s\n' "$OPENAPI_FILE" >&2
    exit 2
fi
if [[ ! -f "$PROTO_FILE" ]]; then
    printf 'ERROR: proto file missing: %s\n' "$PROTO_FILE" >&2
    exit 2
fi

# Extract message field names from the proto.
#
# A proto message field looks like:
#   [repeated ] <type> <name> = <tag>;
# where <type> may be a primitive (string, int64, bytes, ...), a qualified
# message (foo.Bar), or a fully-qualified enum. We require two whitespace-
# separated tokens before `=` to filter out enum values (which have only one
# word: `CONFIG_STATUS_PENDING = 1;`).
fields=$(grep -oP '^\s*(?:repeated\s+|optional\s+|required\s+)?[\w.]+\s+\K\w+(?=\s*=\s*\d+\s*;)' \
         "$PROTO_FILE" | sort -u)

if [[ -z "$fields" ]]; then
    printf 'ERROR: no proto fields extracted from %s (regex broken?)\n' \
        "$PROTO_FILE" >&2
    exit 2
fi

missing=()
while IFS= read -r field; do
    [[ -z "$field" ]] && continue
    if ! grep -qE "^\s*${field}\s*:" "$OPENAPI_FILE"; then
        missing+=("$field")
    fi
done <<< "$fields"

total=$(wc -l <<< "$fields")

if (( ${#missing[@]} > 0 )); then
    {
        printf 'FAIL: %d proto field(s) missing from %s:\n' \
            "${#missing[@]}" "$OPENAPI_FILE"
        printf '  - %s\n' "${missing[@]}"
    } >&2
    exit 1
fi

printf 'OK: all %d proto field names appear in %s\n' "$total" "$OPENAPI_FILE"
exit 0
