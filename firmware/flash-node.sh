#!/usr/bin/env bash
#
# flash-node.sh - flash the Mari node app onto connected nRF52840
# boards/dongles over SWD (J-Link). Run from firmware/.
#
# Flash only by default - no compile. Pass --build to (re)build first, or
# --hex <file> to flash a hex you provide. Without --hex, the default is the
# node app built from mari-node-nrf52840dk.emProject (project 03app_node,
# target nrf52840dk):
#   app/03app_node/Output/nrf52840dk/<config>/Exe/03app_node-nrf52840dk.hex
# SES emits a .hex (no .bin); nrfjprog programs the hex directly, taking the
# load address from the file.
#
# Dongles (PCA10059) have no on-board debugger: wire an external J-Link to the
# dongle's SWD pads, then run this. Flashing over SWD writes the app from 0x0
# and overwrites the factory USB bootloader - that's expected; the bare-metal
# node app boots directly from reset. (USB-DFU flashing via nrfutil is a
# separate path this script does NOT do.) LEDs/buttons are mapped for the DK,
# so on a dongle they may not light correctly; the radio/node logic is identical.
#
# Factory dongles ship with readback protection (APPROTECT) on - they read as
# UNKNOWN and can't be flashed until unlocked. A locked target is auto-recovered
# (nrfjprog --recover, a full chip erase) with a warning, then flashed - no flag
# needed. Heads up: a locked board is indistinguishable from a locked non-node
# board until after the erase, so auto-recover can also wipe a locked nRF5340;
# identified (readable) non-nRF52840 boards are still skipped, never recovered.
# Pass --recover to force a clean-slate recover on every target, even ones that
# are already unlocked.
#
# Usage:
#   ./flash-node.sh --all                  flash connected nRF52840s (auto-unlocks locked ones)
#   ./flash-node.sh --recover --all        force a clean-slate recover + flash
#   ./flash-node.sh <snr> [<snr> ...]      flash specific J-Link serials
#   ./flash-node.sh                        list connected devices, then exit
#
# Options:
#   --hex <file> flash this hex instead of the repo default
#   --build      (re)build the node app first (default: flash only, no compile)
#   --recover    force nrfjprog --recover on every target (locked ones recover anyway)
#   --force      flash even if the target isn't detected as an nRF52840
#   --dry-run    do everything except the actual nrfjprog --program / --recover
#
# Env:
#   SEGGER_DIR    SES install used for --build (default: /opt/segger)
#   BUILD_CONFIG  SES build config used for --build and the default hex path (default: Debug)

set -euo pipefail

SEGGER_DIR="${SEGGER_DIR:-/opt/segger}"
BUILD_CONFIG="${BUILD_CONFIG:-Debug}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$SCRIPT_DIR"

DO_BUILD=0
FORCE=0
DRY_RUN=0
RECOVER=0
HEX_FILE=""
TARGETS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)    DO_BUILD=1 ;;
    --recover)  RECOVER=1 ;;
    --force)    FORCE=1 ;;
    --dry-run)  DRY_RUN=1 ;;
    --hex)      shift; HEX_FILE="${1:-}"; [[ -z "$HEX_FILE" ]] && { echo "Error: --hex needs a path" >&2; exit 2; } ;;
    --hex=*)    HEX_FILE="${1#--hex=}" ;;
    --all)      TARGETS=("--all") ;;
    -h|--help)  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
    -*)         echo "Unknown option: $1" >&2; exit 2 ;;
    *)          TARGETS+=("$1") ;;
  esac
  shift
done

# Default to the node app built from mari-node-nrf52840dk.emProject (03app_node).
HEX_FILE="${HEX_FILE:-$FW_DIR/app/03app_node/Output/nrf52840dk/$BUILD_CONFIG/Exe/03app_node-nrf52840dk.hex}"

command -v nrfjprog >/dev/null || { echo "Error: nrfjprog not found in PATH" >&2; exit 1; }

CONNECTED="$(nrfjprog --ids 2>/dev/null | grep -oE '[0-9]+' || true)"
if [[ -z "$CONNECTED" ]]; then
  echo "Error: no nRF devices connected (check the J-Link cable)." >&2
  exit 1
fi

# No selector given: show what's on the bench and bail.
if [[ ${#TARGETS[@]} -eq 0 ]]; then
  echo "Connected J-Link serials:"
  while read -r snr; do
    [[ -z "$snr" ]] && continue
    fam="$(nrfjprog --snr "$snr" --deviceversion 2>/dev/null || echo '???')"
    printf '  %s  (%s)\n' "$snr" "$fam"
  done <<< "$CONNECTED"
  echo
  echo "Re-run with --all or one/more of the serials above."
  exit 0
fi

# Resolve the target list.
if [[ "${TARGETS[0]}" == "--all" ]]; then
  TO_FLASH=()
  while IFS= read -r snr; do
    [[ -n "$snr" ]] && TO_FLASH+=("$snr")
  done <<< "$CONNECTED"
else
  TO_FLASH=()
  for want in "${TARGETS[@]}"; do
    if grep -qx "$want" <<< "$CONNECTED"; then
      TO_FLASH+=("$want")
    else
      echo "Error: device '$want' is not connected. Available:" >&2
      echo "$CONNECTED" | sed 's/^/  /' >&2
      exit 1
    fi
  done
fi

# Build the node app (unless skipped).
if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "Building Mari node app ($BUILD_CONFIG) ..."
  SEGGER_DIR="$SEGGER_DIR" BUILD_CONFIG="$BUILD_CONFIG" make -C "$FW_DIR" node
fi

if [[ ! -f "$HEX_FILE" ]]; then
  echo "Error: hex not found: $HEX_FILE" >&2
  echo "       (pass --hex <file>, or --build to compile it first)" >&2
  exit 1
fi

echo
echo "Flash plan:"
echo "  hex:     $HEX_FILE"
echo "  targets: ${TO_FLASH[*]}"
echo

FLASHED=0
SKIPPED=0
for snr in "${TO_FLASH[@]}"; do
  fam="$(nrfjprog --snr "$snr" --deviceversion 2>/dev/null || echo 'UNKNOWN')"

  # Identified, but not an nRF52840 (e.g. the nRF5340 gateway): never touch it
  # unless forced. Checked before any recover so a known gateway can't get wiped.
  if [[ "$fam" != UNKNOWN && "$fam" != NRF52840* && "$FORCE" -ne 1 ]]; then
    echo "SKIP $snr: target is '$fam', not an nRF52840 (use --force to override)."
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  # Decide whether to recover. A locked target reads UNKNOWN and must be unlocked
  # before it can be flashed, so auto-recover it (with a warning). --recover forces
  # a clean-slate recover even on a readable device.
  recover_this=0
  if [[ "$fam" == UNKNOWN ]]; then
    echo "WARN $snr: locked/unreadable (APPROTECT?); auto-recovering - this is a full chip erase."
    recover_this=1
  elif [[ "$RECOVER" -eq 1 ]]; then
    recover_this=1
  fi

  if [[ "$recover_this" -eq 1 ]]; then
    echo "Recovering $snr (full chip erase, clears APPROTECT) ..."
    rcmd=(nrfjprog --snr "$snr" --recover)
    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "  [dry-run] ${rcmd[*]}"
      [[ "$fam" == UNKNOWN ]] && fam="NRF52840 (assumed after recover)"
    else
      "${rcmd[@]}"
      fam="$(nrfjprog --snr "$snr" --deviceversion 2>/dev/null || echo 'UNKNOWN')"
    fi
  fi

  # Final family gate, now that a recover may have made the part readable.
  if [[ "$fam" != NRF52840* && "$FORCE" -ne 1 ]]; then
    echo "SKIP $snr: target is '$fam', not an nRF52840 (use --force to override)."
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  echo "Flashing $snr ($fam) ..."
  cmd=(nrfjprog --snr "$snr" --program "$HEX_FILE" --sectorerase --verify --reset)
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "  [dry-run] ${cmd[*]}"
  else
    "${cmd[@]}"
  fi
  FLASHED=$((FLASHED + 1))
done

echo
echo "Done. flashed=$FLASHED skipped=$SKIPPED"
