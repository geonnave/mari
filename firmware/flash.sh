#!/usr/bin/env bash
#
# flash.sh - flash Mari firmware onto connected nRF boards over SWD (J-Link).
# Run from firmware/. Two roles:
#   node     -> nRF52840, single core, app 03app_node
#   gateway  -> nRF5340,  dual core,   03app_gateway_app (app) + 03app_gateway_net (net)
#
# Flash only by default - no compile. Pass --build to (re)build the role first,
# or override the image(s) with --hex (node) / --app-hex + --net-hex (gateway).
# Without an override the defaults are the images the role's .emProject(s) build:
#   node:    app/03app_node/Output/nrf52840dk/<cfg>/Exe/03app_node-nrf52840dk.hex
#   gateway: app/03app_gateway_app/Output/nrf5340-app/<cfg>/Exe/03app_gateway_app-nrf5340-app.hex
#            app/03app_gateway_net/Output/nrf5340-net/<cfg>/Exe/03app_gateway_net-nrf5340-net.hex
# SES emits a .hex (no .bin); nrfjprog programs the hex directly.
#
# nRF52840 dongles (PCA10059) have no on-board debugger: wire an external J-Link
# to the SWD pads. Flashing over SWD writes from 0x0 and overwrites the factory
# USB bootloader - that's expected; the bare-metal app boots from reset.
#
# Locked boards: factory parts ship with readback protection (APPROTECT) on and
# read as UNKNOWN. A locked target is auto-recovered (full chip erase) with a
# warning, then flashed. The nRF5340 gateway is dual-core, so it is recovered
# and programmed on both the application and network cores; mirroring the
# behaviour of `dotbot device`, the gateway is recovered on every flash unless
# you pass --no-recover.
#
# Family guard: a `node` run only touches nRF52840s and a `gateway` run only
# touches nRF5340s; a board identified as the wrong family is skipped (use
# --force to override). The guard runs before any recover, so flashing nodes
# can't wipe a gateway and vice versa. A locked (UNKNOWN) board can't be
# identified until after the erase, so it is recovered under the role you asked
# for.
#
# Usage:
#   ./flash.sh <node|gateway|both> --all              flash every matching connected board
#   ./flash.sh <node|gateway|both> <snr> [<snr> ...]  flash specific J-Link serials
#   ./flash.sh <node|gateway|both>                    list connected devices, then exit
#
# `both` runs the gateway pass then the node pass with the same options. Since
# each pass only touches its own family, `./flash.sh both --all --build` builds
# and flashes everything connected in one command, routing each board by the
# family it reports.
#
# Options:
#   --hex <file>      node only:    flash this hex instead of the default
#   --app-hex <file>  gateway only: app-core hex override
#   --net-hex <file>  gateway only: net-core hex override
#   --schedule <s>    gateway only: net-core image for the tiny|medium|big|huge
#                     schedule, from the Output/schedules/ cache that
#                     build-schedules.sh fills. The schedule is a compile-time
#                     pointer, so naming it here is the same thing as naming
#                     the image it produced. Refuses to flash an image older
#                     than the firmware sources: add --build to rebuild that
#                     one image first, or --force to flash it as it is.
#   --net-id <hex>    gateway only: provision the network id (16-bit hex, e.g.
#                     a000) into the net core's config page after programming,
#                     overriding the compile-time fallback. The value is read
#                     back and printed. Nodes have no such page; theirs is
#                     MARI_APP_NET_ID in 03app_node/main.c
#   --erase-only      erase only: recover the targets and stop, no programming.
#                     On the dual-core gateway that is both cores, which is the
#                     pair of nrfjprog calls you would otherwise run by hand.
#   --build           (re)build first (default: flash only, no compile). With
#                     --schedule that is the one cached image, via
#                     build-schedules.sh, rather than the role's default output
#   --recover         force a clean-slate recover before flashing
#   --no-recover      skip recover even on the gateway (which recovers by default)
#   --force           flash anyway when a check would stop you: a target that
#                     isn't the role's expected family, or a stale --schedule image
#   --dry-run         print every nrfjprog command without running it
#
# Env:
#   SEGGER_DIR    SES install used for --build (default: /opt/segger)
#   BUILD_CONFIG  SES config for --build and the default hex paths (default: Debug)

set -euo pipefail

SEGGER_DIR="${SEGGER_DIR:-/opt/segger}"
BUILD_CONFIG="${BUILD_CONFIG:-Debug}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$SCRIPT_DIR"

DO_BUILD=0
ERASE=0
FORCE=0
DRY_RUN=0
RECOVER=0
NO_RECOVER=0
ALL=0
ROLE=""
HEX_NODE=""
HEX_APP=""
HEX_NET=""
SCHEDULE=""
NET_ID=""
TARGETS=()

# The gateway net core reads its network id from a packed
# {magic, has_net_id, net_id} of uint32 in the last 2 KB page of its own flash,
# falling back to a compile-time constant when the magic is absent
# (03app_gateway_net/main.c). Writing it here is what makes one image usable on
# any network.
NET_CFG_ADDR="0x0103F800"
NET_CFG_MAGIC="0x5753524D"
PASSTHRU=()

while [[ $# -gt 0 ]]; do
  # A flag that takes a value consumes two argv items, and `both` replays what
  # it did not consume itself, so the flag and its value have to be recorded
  # together or the replayed pass sees a bare value and reads it as a serial.
  arg="$1"
  took_value=0
  case "$1" in
    --build)      DO_BUILD=1 ;;
    --erase-only) ERASE=1 ;;
    --recover)    RECOVER=1 ;;
    --no-recover) NO_RECOVER=1 ;;
    --force)      FORCE=1 ;;
    --dry-run)    DRY_RUN=1 ;;
    --all)        ALL=1 ;;
    --hex)        shift; HEX_NODE="${1:-}"; took_value=1; [[ -z "$HEX_NODE" ]] && { echo "Error: --hex needs a path" >&2; exit 2; } ;;
    --hex=*)      HEX_NODE="${1#--hex=}" ;;
    --app-hex)    shift; HEX_APP="${1:-}"; took_value=1; [[ -z "$HEX_APP" ]] && { echo "Error: --app-hex needs a path" >&2; exit 2; } ;;
    --app-hex=*)  HEX_APP="${1#--app-hex=}" ;;
    --net-hex)    shift; HEX_NET="${1:-}"; took_value=1; [[ -z "$HEX_NET" ]] && { echo "Error: --net-hex needs a path" >&2; exit 2; } ;;
    --net-hex=*)  HEX_NET="${1#--net-hex=}" ;;
    --schedule)   shift; SCHEDULE="${1:-}"; took_value=1; [[ -z "$SCHEDULE" ]] && { echo "Error: --schedule needs one of tiny|medium|big|huge" >&2; exit 2; } ;;
    --schedule=*) SCHEDULE="${1#--schedule=}" ;;
    --net-id)     shift; NET_ID="${1:-}"; took_value=1; [[ -z "$NET_ID" ]] && { echo "Error: --net-id needs a 16-bit hex value, e.g. a000" >&2; exit 2; } ;;
    --net-id=*)   NET_ID="${1#--net-id=}" ;;
    -h|--help)    awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
    node|gateway|both) [[ -n "$ROLE" ]] && { echo "Error: role already set to '$ROLE'" >&2; exit 2; }; ROLE="$1" ;;
    -*)           echo "Unknown option: $1" >&2; exit 2 ;;
    *)            TARGETS+=("$1") ;;
  esac
  # Everything except the role is replayed verbatim by the `both` pass below.
  case "$arg" in
    node|gateway|both) ;;
    *) if [[ "$took_value" -eq 1 ]]; then PASSTHRU+=("$arg" "$1"); else PASSTHRU+=("$arg"); fi ;;
  esac
  shift
done

if [[ -z "$ROLE" ]]; then
  echo "Error: first argument must be a role: node | gateway | both" >&2
  echo "  e.g. ./flash.sh node --all   /   ./flash.sh both --all --build" >&2
  exit 2
fi

# `both` is the two passes back to back. Re-invoking rather than looping inside
# keeps every per-role code path below exactly as it is when run alone, and the
# family guard means each board is picked up by the pass that matches it.
# set -e stops at the first failing pass.
if [[ "$ROLE" == both ]]; then
  for r in gateway node; do
    echo "================================ $r ================================"
    # An image flag belongs to one role and is an error in the other, so each
    # pass gets the arguments that mean something to it rather than all of them.
    ARGS=()
    SKIP_VALUE=0
    if [[ ${#PASSTHRU[@]} -gt 0 ]]; then
      for a in "${PASSTHRU[@]}"; do
        if [[ "$SKIP_VALUE" -eq 1 ]]; then SKIP_VALUE=0; continue; fi
        case "$r:$a" in
          node:--app-hex|node:--net-hex|node:--schedule|node:--net-id|gateway:--hex) SKIP_VALUE=1; continue ;;
          node:--app-hex=*|node:--net-hex=*|node:--schedule=*|node:--net-id=*|gateway:--hex=*) continue ;;
        esac
        ARGS+=("$a")
      done
    fi
    if [[ ${#ARGS[@]} -gt 0 ]]; then
      "$0" "$r" "${ARGS[@]}"
    else
      "$0" "$r"
    fi
    echo
  done
  exit 0
fi

if [[ "$RECOVER" -eq 1 && "$NO_RECOVER" -eq 1 ]]; then
  echo "Error: --recover and --no-recover are mutually exclusive" >&2
  exit 2
fi

if [[ "$ERASE" -eq 1 && "$NO_RECOVER" -eq 1 ]]; then
  echo "Error: --erase-only and --no-recover contradict each other" >&2
  exit 2
fi

if [[ "$ERASE" -eq 1 && "$DO_BUILD" -eq 1 ]]; then
  echo "Error: --erase-only does not program anything, so --build has nothing to do" >&2
  exit 2
fi

# Per-role config. Family / coprocessor values mirror dotbot device's flash
# engine, which derives them from the .emProject device defines.
case "$ROLE" in
  node)
    EXPECT_GLOB="NRF52840*"
    NRF_FAMILY="NRF52"
    MULTICORE=0
    MAKE_TARGET="node"
    [[ -n "$HEX_APP$HEX_NET" ]] && { echo "Error: --app-hex/--net-hex are gateway-only; use --hex for node" >&2; exit 2; }
    [[ -n "$SCHEDULE" ]] && { echo "Error: --schedule is gateway-only; nodes adopt whatever the beacon advertises" >&2; exit 2; }
    [[ -n "$NET_ID" ]] && { echo "Error: --net-id is gateway-only; the node's is a compile-time constant (MARI_APP_NET_ID)" >&2; exit 2; }
    HEX_APP="${HEX_NODE:-$FW_DIR/app/03app_node/Output/nrf52840dk/$BUILD_CONFIG/Exe/03app_node-nrf52840dk.hex}"
    HEX_NET=""
    ;;
  gateway)
    EXPECT_GLOB="NRF5340*"
    NRF_FAMILY="NRF53"
    MULTICORE=1
    MAKE_TARGET="gateway"
    [[ -n "$HEX_NODE" ]] && { echo "Error: --hex is node-only; use --app-hex/--net-hex for gateway" >&2; exit 2; }
    if [[ -n "$SCHEDULE" ]]; then
      [[ -n "$HEX_NET" ]] && { echo "Error: --schedule and --net-hex both name the net-core image; pass one" >&2; exit 2; }
      case "$SCHEDULE" in
        tiny|medium|big|huge) ;;
        *) echo "Error: unknown schedule '$SCHEDULE' (tiny|medium|big|huge)" >&2; exit 2 ;;
      esac
      HEX_NET="$FW_DIR/Output/schedules/03app_gateway_net-$SCHEDULE.hex"
    fi
    if [[ -n "$NET_ID" ]]; then
      [[ "$ERASE" -eq 1 ]] && { echo "Error: --erase-only leaves no firmware to read a network id" >&2; exit 2; }
      NET_ID="${NET_ID#0x}"; NET_ID="${NET_ID#0X}"
      if ! [[ "$NET_ID" =~ ^[0-9a-fA-F]{1,4}$ ]]; then
        echo "Error: --net-id takes a 16-bit hex value, e.g. a000 or 0xA000; got '$NET_ID'" >&2
        exit 2
      fi
      # printf rather than ${x^^}: bash 3.2 (macOS) has no case conversion.
      NET_ID="$(printf '%04X' "0x$NET_ID")"
      [[ "$NET_ID" == "0000" ]] && { echo "Error: --net-id 0 is not a network" >&2; exit 2; }
    fi
    HEX_APP="${HEX_APP:-$FW_DIR/app/03app_gateway_app/Output/nrf5340-app/$BUILD_CONFIG/Exe/03app_gateway_app-nrf5340-app.hex}"
    HEX_NET="${HEX_NET:-$FW_DIR/app/03app_gateway_net/Output/nrf5340-net/$BUILD_CONFIG/Exe/03app_gateway_net-nrf5340-net.hex}"
    ;;
esac

command -v nrfjprog >/dev/null || { echo "Error: nrfjprog not found in PATH" >&2; exit 1; }

CONNECTED="$(nrfjprog --ids 2>/dev/null | grep -oE '[0-9]+' || true)"
if [[ -z "$CONNECTED" ]]; then
  echo "Error: no nRF devices connected (check the J-Link cable)." >&2
  exit 1
fi

# No selector given: show what's on the bench and bail. Listed per role, the
# same way flashing treats them: only this role's family is a candidate, and a
# locked board counts as one because it cannot be identified until it has been
# recovered. Everything else is reported separately, so a missing board is
# distinguishable from one plugged in under the other role.
if [[ "$ALL" -eq 0 && ${#TARGETS[@]} -eq 0 ]]; then
  MATCHES=()
  OTHERS=()
  while read -r snr; do
    [[ -z "$snr" ]] && continue
    fam="$(nrfjprog --snr "$snr" --deviceversion 2>/dev/null || echo 'UNKNOWN')"
    if [[ "$fam" == $EXPECT_GLOB ]]; then
      MATCHES+=("$snr  ($fam)")
    elif [[ "$fam" == UNKNOWN ]]; then
      MATCHES+=("$snr  (locked or unreadable; would be recovered as $ROLE)")
    else
      OTHERS+=("$snr  ($fam)")
    fi
  done <<< "$CONNECTED"

  if [[ ${#MATCHES[@]} -gt 0 ]]; then
    echo "Connected $EXPECT_GLOB devices for role '$ROLE':"
    printf '  %s\n' "${MATCHES[@]}"
    echo
    echo "Re-run with --all or one/more of the serials above."
  else
    echo "No $EXPECT_GLOB device connected for role '$ROLE'."
  fi

  if [[ ${#OTHERS[@]} -gt 0 ]]; then
    echo
    echo "Not for this role (wrong family, would be skipped):"
    printf '  %s\n' "${OTHERS[@]}"
  fi
  exit 0
fi

# Resolve the target list.
TO_FLASH=()
if [[ "$ALL" -eq 1 ]]; then
  while IFS= read -r snr; do
    [[ -n "$snr" ]] && TO_FLASH+=("$snr")
  done <<< "$CONNECTED"
else
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

# Build the role (unless skipped). With --schedule the target is one net-core
# image in the cache rather than the role's default output, and only
# build-schedules.sh knows how to produce that: it flips the compile-time
# schedule pointer, builds, and files the result under its schedule's name.
if [[ "$DO_BUILD" -eq 1 && "$ERASE" -eq 0 ]]; then
  if [[ -n "$SCHEDULE" ]]; then
    echo "Building the $SCHEDULE net-core image ($BUILD_CONFIG) ..."
    SEGGER_DIR="$SEGGER_DIR" BUILD_CONFIG="$BUILD_CONFIG" "$FW_DIR/build-schedules.sh" "$SCHEDULE"
  else
    echo "Building Mari $ROLE ($BUILD_CONFIG) ..."
    SEGGER_DIR="$SEGGER_DIR" BUILD_CONFIG="$BUILD_CONFIG" make -C "$FW_DIR" "$MAKE_TARGET"
  fi
fi

# A cached schedule image is only as current as the last build-schedules.sh
# run, and flashing a stale one puts old firmware on the gateway without
# leaving a trace in the measurements it goes on to produce. So this stops
# rather than warns: a campaign runner invokes this script with its output
# captured and shows it only on a non-zero exit, where a warning would be seen
# by nobody.
if [[ -n "$SCHEDULE" && "$ERASE" -eq 0 ]]; then
  if [[ ! -f "$HEX_NET" ]]; then
    echo "Error: no image for the $SCHEDULE schedule at $HEX_NET" >&2
    echo "       Re-run with --build, or build it: $FW_DIR/build-schedules.sh $SCHEDULE" >&2
    exit 1
  fi
  NEWER="$(find "$FW_DIR/app" "$FW_DIR/mari" "$FW_DIR/drv" "$FW_DIR/nRF" \
                -type d -name Output -prune -o \
                -type f \( -name '*.c' -o -name '*.h' -o -name '*.emProject' \) \
                -newer "$HEX_NET" -print 2>/dev/null | head -n 1 || true)"
  if [[ -n "$NEWER" && "$FORCE" -eq 0 ]]; then
    echo "Error: the $SCHEDULE image is older than the firmware sources." >&2
    echo "       image:  $(date -r "$HEX_NET" '+%Y-%m-%d %H:%M')  ${HEX_NET#"$FW_DIR"/}" >&2
    echo "       source: $(date -r "$NEWER" '+%Y-%m-%d %H:%M')  ${NEWER#"$FW_DIR"/}" >&2
    echo "       Re-run with --build to rebuild it, or --force to flash it as it is." >&2
    exit 1
  fi
  echo "net core: $SCHEDULE schedule, built $(date -r "$HEX_NET" '+%Y-%m-%d %H:%M')"
fi

# Hex presence check. Skipped when erasing: there is nothing to program, and
# requiring a build artifact to wipe a board would be nonsense.
if [[ "$ERASE" -eq 0 ]]; then
  for h in "$HEX_APP" $HEX_NET; do
    if [[ ! -f "$h" ]]; then
      echo "Error: hex not found: $h" >&2
      echo "       (pass an explicit hex, or --build to compile it first)" >&2
      exit 1
    fi
  done
fi

# Identify each image by name AND content: two builds of the same source land
# at the same path, so only the checksum tells you whether the boards in front
# of you are running the same binary.
hex_line() {
  printf '  %-9s %s\n' "$1" "$(basename "$2")"
  printf '            %s  %s\n' "$(shasum -a 256 "$2" | cut -c1-16)" "$(dirname "$2")"
}

echo
if [[ "$ERASE" -eq 1 ]]; then
  echo "Erase plan:"
  echo "  role:     $ROLE ($NRF_FAMILY)"
  echo "  action:   full chip erase$([[ "$MULTICORE" -eq 1 ]] && echo ' on BOTH cores'), no programming"
  echo "  targets:  ${TO_FLASH[*]}"
else
  echo "Flash plan:"
  echo "  role:     $ROLE ($NRF_FAMILY)"
  hex_line "app hex:" "$HEX_APP"
  [[ -n "$HEX_NET" ]] && hex_line "net hex:" "$HEX_NET"
  echo "  targets:  ${TO_FLASH[*]}"
fi
echo

# Run a command, or just print it under --dry-run.
run_cmd() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "  [dry-run] $*"
  else
    "$@"
  fi
}

# Recover a target (full chip erase, clears APPROTECT). On the dual-core nRF5340
# both cores are recovered; on the single-core nRF52 there is one core.
recover_target() {
  local snr="$1"
  if [[ "$MULTICORE" -eq 1 ]]; then
    run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --recover --coprocessor CP_APPLICATION
    run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --recover --coprocessor CP_NETWORK
  else
    run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --recover
  fi
}

FLASHED=0
ERASED=0
SKIPPED=0
for snr in "${TO_FLASH[@]}"; do
  fam="$(nrfjprog --snr "$snr" --deviceversion 2>/dev/null || echo 'UNKNOWN')"

  # Identified, but the wrong family for this role: never touch it unless forced.
  # Checked before any recover so a node run can't wipe a gateway (or vice versa).
  if [[ "$fam" != UNKNOWN && "$fam" != $EXPECT_GLOB && "$FORCE" -ne 1 ]]; then
    echo "SKIP $snr: target is '$fam', not a(n) $EXPECT_GLOB for role '$ROLE' (use --force)."
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  # Decide whether to recover. --erase-only is exactly "recover and stop".
  recover_this=0
  if [[ "$ERASE" -eq 1 ]]; then
    recover_this=1
  elif [[ "$NO_RECOVER" -eq 1 ]]; then
    recover_this=0
    [[ "$fam" == UNKNOWN ]] && echo "WARN $snr: locked (APPROTECT?) and --no-recover given; flashing will likely fail."
  elif [[ "$RECOVER" -eq 1 ]]; then
    recover_this=1
  elif [[ "$ROLE" == gateway ]]; then
    recover_this=1
  elif [[ "$fam" == UNKNOWN ]]; then
    echo "WARN $snr: locked/unreadable (APPROTECT?); auto-recovering - this is a full chip erase."
    recover_this=1
  fi

  if [[ "$recover_this" -eq 1 ]]; then
    echo "Recovering $snr (full chip erase, clears APPROTECT) ..."
    recover_target "$snr"
    if [[ "$DRY_RUN" -eq 1 ]]; then
      [[ "$fam" == UNKNOWN ]] && fam="${EXPECT_GLOB%\*} (assumed after recover)"
    else
      fam="$(nrfjprog --snr "$snr" --deviceversion 2>/dev/null || echo 'UNKNOWN')"
    fi
  fi

  # Final family gate, now that a recover may have made the part readable.
  if [[ "$fam" != $EXPECT_GLOB && "$FORCE" -ne 1 ]]; then
    echo "SKIP $snr: target is '$fam', not a(n) $EXPECT_GLOB for role '$ROLE' (use --force)."
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  if [[ "$ERASE" -eq 1 ]]; then
    ERASED=$((ERASED + 1))
    continue
  fi

  echo "Flashing $snr ($fam) as $ROLE ..."
  if [[ "$MULTICORE" -eq 1 ]]; then
    # App core first, then net core - mirrors dotbot device's gateway flow.
    run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_APPLICATION --program "$HEX_APP" --chiperase --verify --reset
    run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_NETWORK --program "$HEX_NET" --chiperase --verify --reset
    if [[ -n "$NET_ID" ]]; then
      # The page is erased explicitly rather than relying on the --chiperase
      # above, so provisioning behaves the same whether or not this run also
      # reprogrammed the core. The reset at the end is what makes the net core
      # read the id it was just given.
      echo "  provisioning network id 0x$NET_ID ..."
      run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_NETWORK --erasepage "$NET_CFG_ADDR"
      run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_NETWORK --memwr "$NET_CFG_ADDR" --val "$NET_CFG_MAGIC"
      run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_NETWORK --memwr "$(printf '0x%08X' $((NET_CFG_ADDR + 4)))" --val 0x00000001
      run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_NETWORK --memwr "$(printf '0x%08X' $((NET_CFG_ADDR + 8)))" --val "0x0000$NET_ID"
      run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_NETWORK --memrd "$NET_CFG_ADDR" --n 12
      run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --coprocessor CP_NETWORK --reset
    fi
  else
    run_cmd nrfjprog -f "$NRF_FAMILY" -s "$snr" --program "$HEX_APP" --sectorerase --verify --reset
  fi
  FLASHED=$((FLASHED + 1))
done

echo
if [[ "$ERASE" -eq 1 ]]; then
  echo "Done. erased=$ERASED skipped=$SKIPPED"
else
  echo "Done. flashed=$FLASHED skipped=$SKIPPED"
fi
