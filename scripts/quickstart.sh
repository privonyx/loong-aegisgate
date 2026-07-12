#!/usr/bin/env bash
# AegisGate quickstart launcher — one-line local docker run for 5-min demo.
#
# Usage:
#   export OPENAI_API_KEY=sk-...
#   bash scripts/quickstart.sh
#
# Env overrides:
#   AEGISGATE_IMAGE (default: aegisgate:latest)
#   AEGISGATE_PORT  (default: 8080)
set -euo pipefail

IMAGE="${AEGISGATE_IMAGE:-aegisgate:latest}"
PORT="${AEGISGATE_PORT:-8080}"

if [[ -z "${OPENAI_API_KEY:-}" ]]; then
    cat <<EOF >&2
[quickstart] ERROR: OPENAI_API_KEY env var not set.
[quickstart] Set it first:
    export OPENAI_API_KEY=sk-...
[quickstart] Then re-run:
    bash scripts/quickstart.sh
EOF
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "[quickstart] ERROR: docker not found on PATH. Install Docker first." >&2
    exit 1
fi

echo "[quickstart] Starting AegisGate quickstart on http://localhost:${PORT}/" >&2
echo "[quickstart] Image: ${IMAGE}" >&2
echo "[quickstart] Read the startup banner for your auto-generated API key." >&2
echo "" >&2

exec docker run --rm -it \
    --name aegisgate-quickstart \
    -p "${PORT}:8080" \
    -e OPENAI_API_KEY="${OPENAI_API_KEY}" \
    -v aegisgate-quickstart-data:/app/data \
    --entrypoint /usr/local/bin/quickstart-entrypoint.sh \
    "${IMAGE}"
