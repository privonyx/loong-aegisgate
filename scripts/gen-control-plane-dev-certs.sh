#!/usr/bin/env bash
# Phase 9.3 Epic 6 Task 6.4 — developer-mode TLS / mTLS bootstrap.
#
# Generates a self-signed CA, a server certificate for the control-plane
# gRPC endpoint, and a client certificate for aegisctl. Not for production:
# the keys are unencrypted on disk and the CA has no revocation story.
#
# Usage:
#   ./scripts/gen-control-plane-dev-certs.sh [OUTPUT_DIR]
#
# Defaults OUTPUT_DIR to ./.dev-certs/control-plane/. Creates:
#   ca.key  ca.crt                     — self-signed CA (10 years)
#   server.key  server.crt  server.csr — signed by ca.crt (SAN=localhost,127.0.0.1)
#   client.key  client.crt  client.csr — signed by ca.crt (CN=aegisctl-dev)
#   client.sha256                      — SHA-256 fingerprint for the
#                                        `allowed_client_fingerprints_sha256`
#                                        list in aegisgate-control-plane.yaml
#
# Everything lives under OUTPUT_DIR so a single `rm -rf` resets state.

set -euo pipefail

OUT_DIR="${1:-.dev-certs/control-plane}"
DAYS_CA="3650"
DAYS_LEAF="825"   # WebPKI cap on leaf-cert validity; stays mTLS-toolchain-friendly
KEY_BITS="2048"

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

log() { printf '[gen-certs] %s\n' "$*"; }

# --- CA ----------------------------------------------------------------------
if [[ ! -s ca.key || ! -s ca.crt ]]; then
    log "generating CA (${DAYS_CA} days)"
    openssl genrsa -out ca.key "$KEY_BITS" >/dev/null 2>&1
    openssl req -x509 -new -nodes -key ca.key \
        -subj "/CN=AegisGate Dev CA" \
        -days "$DAYS_CA" -out ca.crt
else
    log "reusing existing CA"
fi

# --- server ------------------------------------------------------------------
log "generating server cert (${DAYS_LEAF} days, SAN=localhost,127.0.0.1,::1)"
openssl genrsa -out server.key "$KEY_BITS" >/dev/null 2>&1

cat > server.cnf <<EOF
[req]
distinguished_name = req_distinguished_name
req_extensions     = v3_req
prompt             = no

[req_distinguished_name]
CN = localhost

[v3_req]
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
IP.1  = 127.0.0.1
IP.2  = ::1
EOF

openssl req -new -key server.key -out server.csr -config server.cnf
openssl x509 -req -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days "$DAYS_LEAF" \
    -extensions v3_req -extfile server.cnf

# --- client ------------------------------------------------------------------
log "generating client cert (${DAYS_LEAF} days, CN=aegisctl-dev)"
openssl genrsa -out client.key "$KEY_BITS" >/dev/null 2>&1

cat > client.cnf <<'EOF'
[req]
distinguished_name = req_distinguished_name
req_extensions     = v3_req
prompt             = no

[req_distinguished_name]
CN = aegisctl-dev

[v3_req]
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = clientAuth
EOF

openssl req -new -key client.key -out client.csr -config client.cnf
openssl x509 -req -in client.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out client.crt -days "$DAYS_LEAF" \
    -extensions v3_req -extfile client.cnf

# --- fingerprint -------------------------------------------------------------
# Lowercase hex, no colons — matches ServerBootstrap::computeCertFingerprintSha256.
openssl x509 -in client.crt -noout -fingerprint -sha256 \
    | awk -F= '{print $2}' \
    | tr -d ':' \
    | tr '[:upper:]' '[:lower:]' > client.sha256

chmod 600 ca.key server.key client.key
chmod 644 ca.crt server.crt client.crt client.sha256

log "done. Suggested YAML snippet:"
cat <<EOF

  tls:
    cert_path: $(pwd)/server.crt
    key_path:  $(pwd)/server.key
    mutual:    true
    allowed_client_fingerprints_sha256:
      - "$(cat client.sha256)"

EOF
