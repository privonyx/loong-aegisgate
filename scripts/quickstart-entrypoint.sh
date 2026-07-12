#!/usr/bin/env bash
# AegisGate quickstart entrypoint.
#
# Auto-generates an API key, prints a startup banner, and execs aegisgate
# with the minimal quickstart config. USE FOR DEVELOPMENT/DEMO ONLY.
# Production deployments must use the standard Dockerfile ENTRYPOINT.
#
# Usage (inside container):
#   /usr/local/bin/quickstart-entrypoint.sh [config.yaml]
#
# Usage (from host via docker run):
#   docker run -p 8080:8080 -e OPENAI_API_KEY=sk-... \
#     --entrypoint /usr/local/bin/quickstart-entrypoint.sh aegisgate:latest
#
# Environment variables:
#   OPENAI_API_KEY (recommended) — your OpenAI key for LLM calls
#   AEGISGATE_PRODUCTION         — if =1, hard-fail (production safety)
#   AEGISGATE_QUICKSTART_KEY_FILE — override key file path (default: /app/data/quickstart-key.txt)
#
# Security requirements:
#   SR1  Generated key >= 32 bytes random + base64 encoded
#   SR2  AEGISGATE_PRODUCTION=1 -> hard fail exit 1 (production guard)
#   SR3  Key file perm = 600 (owner read only)
#   SR4  Startup banner contains "DO NOT use in production" warning
set -euo pipefail

# --- Color helpers (only when TTY) ---
if [[ -t 2 ]]; then
    RED='\033[1;31m'; YELLOW='\033[1;33m'; GREEN='\033[1;32m'; NC='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; NC=''
fi

# --- --help ---
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<EOF
AegisGate Quickstart Entrypoint

Usage:
  docker run -p 8080:8080 -e OPENAI_API_KEY=sk-... \\
    --entrypoint /usr/local/bin/quickstart-entrypoint.sh aegisgate:latest

Environment variables:
  OPENAI_API_KEY                — your OpenAI key (LLM calls fail without it; admin UI still works)
  AEGISGATE_PRODUCTION          — if =1, hard-fail (production safety; NEVER set in production)
  AEGISGATE_QUICKSTART_KEY_FILE — override key file path (default /app/data/quickstart-key.txt)

This entrypoint:
  1. SR2: refuses to run if AEGISGATE_PRODUCTION=1
  2. SR1: auto-generates a random API key (32-byte random -> base64)
  3. SR3: persists the key with file perm 600 so subsequent restarts reuse it
  4. SR4: prints a startup banner (sleeps 3s) — DO NOT use in production
  5. execs ./aegisgate with config/aegisgate.quickstart.yaml (or your override)

See docs/quickstart.md for the full 5-minute tutorial.
EOF
    exit 0
fi

# --- SR2: production guard ---
if [[ "${AEGISGATE_PRODUCTION:-0}" == "1" ]]; then
    printf '%b\n' "${RED}" >&2
    cat >&2 <<'PRODBLOCK'
████████████████████████████████████████████████████████████████
████  ERROR: quickstart image MUST NOT run in production    ████
████  AEGISGATE_PRODUCTION=1 detected.                       ████
████  Use the standard Dockerfile ENTRYPOINT instead:        ████
████      docker run aegisgate:latest config/aegisgate.yaml  ████
████  See: docs/quickstart.md and docs/deployment.md         ████
████████████████████████████████████████████████████████████████
PRODBLOCK
    printf '%b\n' "${NC}" >&2
    exit 1
fi

# --- SR1+SR3: API key generation ---
KEY_FILE="${AEGISGATE_QUICKSTART_KEY_FILE:-/app/data/quickstart-key.txt}"
KEY_DIR=$(dirname "$KEY_FILE")
mkdir -p "$KEY_DIR" 2>/dev/null || true

if [[ -f "$KEY_FILE" && -s "$KEY_FILE" ]]; then
    QUICKSTART_KEY=$(cat "$KEY_FILE")
    printf '%b[quickstart]%b Reusing existing API key from %s\n' "${YELLOW}" "${NC}" "$KEY_FILE" >&2
else
    # SR1: 32 random bytes -> base64 (~44 chars), strip newlines and padding
    QUICKSTART_KEY=$(head -c 32 /dev/urandom | base64 | tr -d '\n=')
    printf '%s' "$QUICKSTART_KEY" > "$KEY_FILE"
    chmod 600 "$KEY_FILE"  # SR3
    printf '%b[quickstart]%b Generated new API key -> %s\n' "${GREEN}" "${NC}" "$KEY_FILE" >&2
fi

# Export key for aegisgate config substitution
export AEGISGATE_QUICKSTART_API_KEY="$QUICKSTART_KEY"
# Legacy compat: also export AEGISGATE_API_KEY so existing config templates work
export AEGISGATE_API_KEY="$QUICKSTART_KEY"

# --- OPENAI_API_KEY check (warn but don't block) ---
if [[ -z "${OPENAI_API_KEY:-}" ]]; then
    printf '%b[quickstart] WARN: OPENAI_API_KEY not set; LLM calls will fail (admin UI still works)%b\n' \
        "${YELLOW}" "${NC}" >&2
fi

# --- SR4: startup banner (sleeps 3s) ---
cat >&2 <<EOF

╔══════════════════════════════════════════════════════════════════╗
║  ⚠️  AegisGate QUICKSTART MODE — development / demo ONLY  ⚠️    ║
║                                                                  ║
║  Quickstart API key (auto-generated):                            ║
║    ${QUICKSTART_KEY}
║                                                                  ║
║  Try it now (after server starts on :8080):                      ║
║    curl -H "Authorization: Bearer ${QUICKSTART_KEY}" \\
║         http://localhost:8080/admin/api/savings/summary
║                                                                  ║
║  DO NOT use in production. See docs/quickstart.md                ║
╚══════════════════════════════════════════════════════════════════╝

EOF

# Give the user time to read the banner (skip if NO_SLEEP for test speed)
if [[ "${AEGISGATE_QUICKSTART_NO_SLEEP:-0}" != "1" ]]; then
    sleep 3
fi

# --- exec aegisgate ---
CONFIG="${1:-config/aegisgate.quickstart.yaml}"
exec ./aegisgate "$CONFIG"
