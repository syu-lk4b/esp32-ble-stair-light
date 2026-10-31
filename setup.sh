#!/usr/bin/env bash
#
# ESP32 BLE Stair Light Panel - interactive installer
#
#   ./setup.sh                 configure, build and flash
#   ./setup.sh --skip-flash    host side only (device already flashed)
#
# Runs on macOS and Linux.  Requires PlatformIO and curl.
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

SECRETS="$REPO_DIR/include/secrets.h"

SKIP_FLASH=0
for arg in "$@"; do
  case "$arg" in
    --skip-flash) SKIP_FLASH=1 ;;
    -h|--help)    sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [ -t 1 ]; then
  B=$'\033[1m'; C=$'\033[36m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; N=$'\033[0m'
else
  B=; C=; G=; Y=; R=; N=
fi

TOTAL=6
STEP=0

step() {
  STEP=$((STEP + 1))
  printf "\n${B}${C}[%d/%d] %s${N}\n" "$STEP" "$TOTAL" "$1"
}
ok()   { printf "  ${G}✓${N} %s\n" "$1"; }
warn() { printf "  ${Y}!${N} %s\n" "$1"; }
die()  { printf "${R}✗ %s${N}\n" "$1" >&2; exit 1; }

ask() {
  local __var="$1" __prompt="$2" __def="${3-}" __ans=""
  if [ -n "$__def" ]; then
    read -r -p "  $__prompt [$__def]: " __ans || true
    __ans="${__ans:-$__def}"
  else
    read -r -p "  $__prompt: " __ans || true
  fi
  printf -v "$__var" '%s' "$__ans"
}

ask_secret() {
  local __var="$1" __prompt="$2" __ans=""
  read -r -s -p "  $__prompt: " __ans || true
  echo
  printf -v "$__var" '%s' "$__ans"
}

confirm() {
  local __prompt="$1" __def="${2:-Y}" __ans=""
  read -r -p "  $__prompt [$__def]: " __ans || true
  __ans="${__ans:-$__def}"
  case "$__ans" in [yY]*) return 0 ;; *) return 1 ;; esac
}

get_define() {
  sed -n "s/^#define[[:space:]][[:space:]]*$1[[:space:]][[:space:]]*\"\\(.*\\)\"/\\1/p" "$SECRETS" 2>/dev/null | head -1
}

get_num() {
  sed -n "s/^#define[[:space:]][[:space:]]*$1[[:space:]][[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "$SECRETS" 2>/dev/null | head -1
}

c_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

printf "${B}ESP32 BLE Stair Light Panel - installer${N}\n"
printf "repo: %s\n" "$REPO_DIR"

# ---------------------------------------------------------------------------
step "Checking prerequisites"
# ---------------------------------------------------------------------------
case "$(uname -s)" in
  Darwin|Linux) ok "$(uname -s)" ;;
  *) die "unsupported OS: $(uname -s)" ;;
esac

command -v pio >/dev/null 2>&1 || die "PlatformIO not found. Install it: https://platformio.org/install"
ok "PlatformIO $(pio --version | head -1 | sed 's/PlatformIO Core, version //')"

command -v curl >/dev/null 2>&1 || die "curl not found"
ok "curl"

# ---------------------------------------------------------------------------
step "Device configuration (include/secrets.h)"
# ---------------------------------------------------------------------------
SKIP_SECRETS=0
if [ -f "$SECRETS" ]; then
  warn "include/secrets.h already exists."
  if confirm "Reuse it and skip the questions?" "Y"; then
    SKIP_SECRETS=1
    WIFI_SSID="$(get_define WIFI_SSID)"
    WIFI_PASS="$(get_define WIFI_PASS)"
    HA_BASE_URL="$(get_define HA_BASE_URL)"
    HA_TOKEN="$(get_define HA_TOKEN)"
    BLE_NAME_DOWN="$(get_define BLE_NAME_DOWN)"; BLE_NAME_DOWN="${BLE_NAME_DOWN:-downstair}"
    BLE_NAME_UP="$(get_define BLE_NAME_UP)"; BLE_NAME_UP="${BLE_NAME_UP:-upstair}"
    TIMER_ON_HOUR="$(get_num TIMER_ON_HOUR)"; TIMER_ON_HOUR="${TIMER_ON_HOUR:-18}"
    TIMER_ON_MIN="$(get_num TIMER_ON_MIN)"; TIMER_ON_MIN="${TIMER_ON_MIN:-0}"
    TIMER_OFF_HOUR="$(get_num TIMER_OFF_HOUR)"; TIMER_OFF_HOUR="${TIMER_OFF_HOUR:-23}"
    TIMER_OFF_MIN="$(get_num TIMER_OFF_MIN)"; TIMER_OFF_MIN="${TIMER_OFF_MIN:-45}"
    WEB_USER="$(get_define WEB_USER)"; WEB_USER="${WEB_USER:-admin}"
    WEB_PASS="$(get_define WEB_PASS)"
    STATIC_IP="$(get_define STATIC_IP)"
    MDNS_HOST="$(get_define MDNS_HOST)"; MDNS_HOST="${MDNS_HOST:-stair-panel}"
    ok "reusing existing secrets.h"
  fi
fi

if [ "$SKIP_SECRETS" -eq 0 ]; then
  ask WIFI_SSID "Wi-Fi SSID (2.4 GHz only)" "${WIFI_SSID:-}"
  [ -n "$WIFI_SSID" ] || die "SSID cannot be empty"
  ask_secret WIFI_PASS "Wi-Fi password"
  [ -n "$WIFI_PASS" ] || die "Wi-Fi password cannot be empty"

  ask HA_BASE_URL "Home Assistant URL" "${HA_BASE_URL:-http://homeassistant.local:8123}"
  HA_BASE_URL="${HA_BASE_URL%/}"
  ask_secret HA_TOKEN "HA long-lived access token"
  [ -n "$HA_TOKEN" ] || warn "no token: the panel will still work, but HA sync will fail"

  ask BLE_NAME_DOWN "BLE name of the downstairs controller" "downstair"
  ask BLE_NAME_UP "BLE name of the upstairs controller" "upstair"

  TIMER_ON_HOUR="${TIMER_ON_HOUR:-18}"; TIMER_ON_MIN="${TIMER_ON_MIN:-0}"
  TIMER_OFF_HOUR="${TIMER_OFF_HOUR:-23}"; TIMER_OFF_MIN="${TIMER_OFF_MIN:-45}"
  if confirm "Use the built-in timer 18:00-23:45?" "Y"; then
    :
  else
    ask TIMER_ON_HOUR "Timer ON hour (0-23)" "$TIMER_ON_HOUR"
    ask TIMER_ON_MIN "Timer ON minute (0-59)" "$TIMER_ON_MIN"
    ask TIMER_OFF_HOUR "Timer OFF hour (0-23)" "$TIMER_OFF_HOUR"
    ask TIMER_OFF_MIN "Timer OFF minute (0-59)" "$TIMER_OFF_MIN"
  fi

  STATIC_IP=""; STATIC_GW=""; STATIC_MASK=""
  if ! confirm "Use DHCP for the panel?" "Y"; then
    ask STATIC_IP "Static IP (e.g. 192.168.1.60)"
    ask STATIC_GW "Gateway (e.g. 192.168.1.1)"
    ask STATIC_MASK "Subnet mask" "255.255.255.0"
  fi

  WEB_USER="admin"; WEB_PASS=""
  if confirm "Protect the web UI / API with a password?" "Y"; then
    WEB_PASS="$(openssl rand -hex 6)"
    ok "generated web password: $WEB_PASS"
    warn "write it down"
  fi

  ask MDNS_HOST "mDNS hostname" "stair-panel"

  if [ -f "$SECRETS" ]; then
    cp "$SECRETS" "$SECRETS.bak.$(date +%Y%m%d-%H%M%S)"
    warn "old secrets backed up next to secrets.h"
  fi

  cat > "$SECRETS" <<EOF
#pragma once

// Generated by setup.sh - DO NOT COMMIT THIS FILE.

#define WIFI_SSID "$(c_escape "$WIFI_SSID")"
#define WIFI_PASS "$(c_escape "$WIFI_PASS")"

#define HA_BASE_URL "$(c_escape "$HA_BASE_URL")"
#define HA_TOKEN "$(c_escape "$HA_TOKEN")"

#define BLE_NAME_DOWN "$(c_escape "$BLE_NAME_DOWN")"
#define BLE_NAME_UP   "$(c_escape "$BLE_NAME_UP")"

#define TIMER_ON_HOUR  $TIMER_ON_HOUR
#define TIMER_ON_MIN   $TIMER_ON_MIN
#define TIMER_OFF_HOUR $TIMER_OFF_HOUR
#define TIMER_OFF_MIN  $TIMER_OFF_MIN

#define WEB_USER "$(c_escape "$WEB_USER")"
#define WEB_PASS "$(c_escape "$WEB_PASS")"

#define STATIC_IP   "$(c_escape "$STATIC_IP")"
#define STATIC_GW   "$(c_escape "$STATIC_GW")"
#define STATIC_MASK "$(c_escape "$STATIC_MASK")"

#define MDNS_HOST "$(c_escape "$MDNS_HOST")"
EOF
  chmod 600 "$SECRETS"
  ok "wrote include/secrets.h (mode 600)"
fi

# ---------------------------------------------------------------------------
step "Build and flash"
# ---------------------------------------------------------------------------
FLASH_FAILED=0
if [ "$SKIP_FLASH" -eq 1 ]; then
  warn "skipped (--skip-flash)"
else
  echo "  Connect the ESP32-S3 panel to this computer with a USB data cable."
  if confirm "Build and flash now?" "Y"; then
    pio run || die "build failed"
    ok "firmware built"
    if [ -n "${UPLOAD_PORT:-}" ]; then
      pio run -t upload --upload-port "$UPLOAD_PORT" || FLASH_FAILED=1
    else
      pio run -t upload || FLASH_FAILED=1
    fi
    if [ "$FLASH_FAILED" -eq 0 ]; then
      ok "firmware flashed"
    else
      warn "flash failed (hold BOOT, tap RESET, or set UPLOAD_PORT=/dev/ttyACM0)"
      if ! confirm "Continue anyway?" "Y"; then
        exit 1
      fi
    fi
  else
    warn "skipped"
  fi
fi

# ---------------------------------------------------------------------------
step "Find the panel on the network"
# ---------------------------------------------------------------------------
DEFAULT_URL="http://$MDNS_HOST.local"
if [ -n "$STATIC_IP" ]; then
  DEFAULT_URL="http://$STATIC_IP"
fi
DEVICE_URL="${DEVICE_URL:-$DEFAULT_URL}"
ask DEVICE_URL "Device URL" "$DEVICE_URL"
DEVICE_URL="${DEVICE_URL%/}"

device_curl() {
  local url="$1"; shift || true
  if [ -n "$WEB_PASS" ]; then
    curl -fsS -m 3 -u "$WEB_USER:$WEB_PASS" "$url" "$@"
  else
    curl -fsS -m 3 "$url" "$@"
  fi
}

STATUS_JSON=""
for _ in $(seq 1 30); do
  if STATUS_JSON="$(device_curl "$DEVICE_URL/status" 2>/dev/null)"; then
    break
  fi
  STATUS_JSON=""
  printf "."
  sleep 2
done
echo

if [ -z "$STATUS_JSON" ]; then
  warn "could not reach $DEVICE_URL"
  warn "try: curl ${WEB_PASS:+-u $WEB_USER:***} $DEVICE_URL/status"
  if ! confirm "Continue anyway?" "Y"; then
    exit 1
  fi
else
  ok "panel online: $DEVICE_URL"
  if command -v python3 >/dev/null 2>&1; then
    printf '%s' "$STATUS_JSON" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print("    firmware : %s" % d.get("fw","?"))
print("    ip       : %s" % d.get("ip","?"))
print("    HA link  : %s" % ("connected" if d.get("ha") else "NOT connected"))
for k in ("downstair","upstair"):
    x=d.get(k,{})
    print("    %-9s: BLE %-11s mode=%s step=%s flow=%s" % (
        k, "connected" if x.get("connected") else "disconnected",
        x.get("mode"), x.get("bri_step"), x.get("bri_flow")))
'
  else
    printf '    %s\n' "$STATUS_JSON"
  fi
fi

# ---------------------------------------------------------------------------
step "Home Assistant helpers"
# ---------------------------------------------------------------------------
cat <<EOF
  Copy the package into Home Assistant:

    cp homeassistant/stair_light_package.yaml <ha-config>/packages/stair_light.yaml

  and make sure configuration.yaml contains:

    homeassistant:
      packages: !include_dir_named packages

  Then restart Home Assistant (or reload YAML).

  Optional:
    - homeassistant/template_lights.example.yaml  -> light entities for HomeKit
    - homeassistant/dashboard_card.yaml           -> dashboard card

  HA URL: $HA_BASE_URL
EOF
if confirm "Open the Home Assistant web UI?" "N"; then
  if command -v open >/dev/null 2>&1; then open "$HA_BASE_URL" 2>/dev/null || true
  elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$HA_BASE_URL" >/dev/null 2>&1 || true
  fi
fi

# ---------------------------------------------------------------------------
step "Smoke test"
# ---------------------------------------------------------------------------
if [ -n "$STATUS_JSON" ]; then
  if confirm "Send a test command (downstairs -> STEP mode)?" "N"; then
    if device_curl -X POST "$DEVICE_URL/set?dev=0&mode=3" >/dev/null 2>&1; then
      ok "command accepted - the downstairs light should switch to STEP"
      sleep 1
      device_curl "$DEVICE_URL/status" 2>/dev/null | head -c 400
      echo
    else
      warn "command failed"
    fi
  else
    warn "skipped"
  fi
fi

printf "\n${B}${G}Done.${N}\n"
printf "  web UI   : %s/\n" "$DEVICE_URL"
printf "  config   : %s\n" "$SECRETS"
printf "  OTA      : pio run && curl -F \"file=@.pio/build/es3c28p/firmware.bin\" %s/ota\n" "$DEVICE_URL"
printf "  protocol : docs/BLE-PROTOCOL.md\n\n"
