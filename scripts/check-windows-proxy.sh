#!/usr/bin/env bash
# Check Windows host proxy reachability (default port 7890) from WSL2.
#
# Used by /van workflow's environment checklist
# (.cursor/rules/workflow/van.md, C4).
#
# Usage:
#   scripts/check-windows-proxy.sh           # default mode (sandbox-safe; static check only)
#   scripts/check-windows-proxy.sh --full    # full mode (PowerShell auto-detect + curl probe)
#   scripts/check-windows-proxy.sh --help    # show this help
#
# Environment variables:
#   WINDOWS_PROXY_HOST                 Override proxy host:port (e.g. "<your_lan_proxy_host>:7890").
#                                      REQUIRED in default mode; optional in --full mode (auto-detect).
#   WINDOWS_PROXY_CHECK_ALLOW_ROOT=1   Escape hatch for SR4 (only set in known-safe contexts
#                                      such as Cursor sandbox or CI containers that always run as root).
#
# Exit codes (per van.md C4 contract):
#   0  PASS     proxy reachable / configured
#   1  WARN     proxy not configured / not reachable / sandbox can't probe
#   2  ERROR    script abort (root run without escape hatch / unrecoverable error)
#
# Security requirements:
#   SR1  No literal LAN IP in this file (use <your_lan_proxy_host> placeholder).
#   SR2  Redact user:pass from URLs before logging.
#   SR3  All probe commands have --max-time / timeout <= 5s.
#   SR4  Refuse to run as root unless WINDOWS_PROXY_CHECK_ALLOW_ROOT=1 is set.
#   SR5  Read-only (no writes outside stdout/stderr).
set -euo pipefail

# --- SR4: refuse to run as root (escape hatch for sandbox/CI) ---
if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    if [[ "${WINDOWS_PROXY_CHECK_ALLOW_ROOT:-0}" != "1" ]]; then
        echo "[ERROR] Do not run as root (SR4). Set WINDOWS_PROXY_CHECK_ALLOW_ROOT=1 to override." >&2
        exit 2
    fi
fi

# --- Color helpers (style: scripts/setup-dev.sh) ---
if [[ -t 1 ]]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; NC=''
fi
log_pass()  { printf '%b[PASS]%b  %s\n' "$GREEN" "$NC" "$*"; }
log_warn()  { printf '%b[WARN]%b  %s\n' "$YELLOW" "$NC" "$*"; }
log_error() { printf '%b[ERROR]%b %s\n' "$RED" "$NC" "$*" >&2; }
log_info()  { printf '[INFO]  %s\n' "$*"; }

# --- SR2: redact user:pass from URLs before logging ---
redact_url() {
    sed -E 's|(://)[^@/]+@|\1[REDACTED]@|g' <<< "$1"
}

show_help() {
    sed -n '2,18p' "$0" | sed 's|^# \?||'
}

# --- Argument parsing ---
MODE="default"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --full)    MODE="full"; shift ;;
        --help|-h) show_help; exit 0 ;;
        *)
            log_error "Unknown option: $1 (use --help)"
            exit 2
            ;;
    esac
done

# --- Probe function (SR3: --max-time 5 mandatory) ---
probe_proxy() {
    local host_port="$1"
    curl --noproxy '*' --silent --show-error --output /dev/null \
         --max-time 5 \
         --write-out 'http_code=%{http_code} time=%{time_total}\n' \
         -x "http://${host_port}" "https://www.google.com" 2>&1
}

# --- Auto-detect via PowerShell (only in --full mode; sandbox-safe in default) ---
detect_proxy_host() {
    if ! command -v powershell.exe >/dev/null 2>&1; then
        return 1
    fi
    local ip
    ip=$(timeout 3 powershell.exe -NoProfile -Command \
        "(Get-NetIPAddress -AddressFamily IPv4 |
          Where-Object { \$_.InterfaceAlias -notmatch 'Loopback|WSL|vEthernet' -and
                         \$_.IPAddress -notmatch '^169\.254' } |
          Select-Object -First 1 -ExpandProperty IPAddress)" \
        2>/dev/null | tr -d '\r\n ' || true)
    if [[ -n "$ip" && "$ip" != "127.0.0.1" ]]; then
        echo "${ip}:7890"
        return 0
    fi
    return 1
}

# --- Main ---
HOST="${WINDOWS_PROXY_HOST:-}"

if [[ -z "$HOST" ]]; then
    if [[ "$MODE" == "default" ]]; then
        log_warn "未配置代理（开发体验受限）。可手动 export WINDOWS_PROXY_HOST=<your_lan_proxy_host>:7890"
        log_info "如需自动探测请用 --full 模式（在 host shell 跑）"
        exit 1
    else
        log_info "未设置 WINDOWS_PROXY_HOST，尝试通过 PowerShell 自动发现 ..."
        if HOST=$(detect_proxy_host); then
            log_info "自动发现 Windows host: $(redact_url "$HOST")"
        else
            log_warn "PowerShell 自动发现失败（sandbox 内不可用 / host 无可用 LAN 接口）"
            log_info "请在 host shell 跑：export WINDOWS_PROXY_HOST=<your_lan_proxy_host>:7890"
            exit 1
        fi
    fi
fi

REDACTED_HOST=$(redact_url "$HOST")

# --- Default mode: only static config check, no network probe ---
if [[ "$MODE" == "default" ]]; then
    log_pass "WINDOWS_PROXY_HOST 已配置: $REDACTED_HOST"
    log_info "（默认模式不做 LAN 网络探测；如需 deep probe 用 --full + host shell）"
    exit 0
fi

# --- Full mode: actual probe ---
log_info "正在探测代理 $REDACTED_HOST ..."
if result=$(probe_proxy "$HOST" 2>&1); then
    code=$(grep -oE 'http_code=[0-9]+' <<< "$result" | head -1 | cut -d= -f2 || true)
    if [[ -n "$code" && "$code" != "000" ]]; then
        log_pass "代理可用: $REDACTED_HOST -> http_code=$code"
        exit 0
    fi
fi
log_warn "代理不可达: $REDACTED_HOST（5s 超时）。检查 Windows 代理软件是否启动 + LAN IP 是否变化"
exit 1
