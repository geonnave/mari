#!/bin/bash

APP=$1
shift

if [[ "$APP" != "node" && "$APP" != "gateway" ]]; then
  echo "Error: Invalid application type"
  echo "Usage: SEGGER_DIR=<segger_dir> $0 <node|gateway> [--all | <device-id1> [device-id2 ...]]"
  echo "Example: SEGGER_DIR=/opt/segger $0 node --all"
  echo "Example: SEGGER_DIR=/opt/segger $0 node 683916736 683916737"
  exit 1
fi

# Get list of connected devices
CONNECTED_DEVICES=$(nrfjprog --ids | grep -oE '[0-9]+' | tr '\n' ' ')
if [ -z "$CONNECTED_DEVICES" ]; then
  echo "Error: No nRF devices connected"
  exit 1
fi

if [ $# -eq 0 ]; then
  echo "Error: No device id numbers provided"
  echo "Usage: $0 <node|gateway> [--all | <device-id1> [device-id2 ...]]"
  echo "Example: $0 node --all"
  echo "Example: $0 node 683916736 683916737"
  echo -e "\nAvailable devices:"
  echo "$CONNECTED_DEVICES"
  exit 1
fi

# Handle --all flag or verify provided ids
if [ "$1" == "--all" ]; then
  DEVICES_TO_FLASH="$CONNECTED_DEVICES"
else
  DEVICES_TO_FLASH=""
  for DEVICE_ID in "$@"; do
    if ! echo "$CONNECTED_DEVICES" | grep -q "${DEVICE_ID}"; then
      echo "Error: Device with id $DEVICE_ID is not connected"
      echo -e "\nAvailable devices:"
      echo "$CONNECTED_DEVICES"
      exit 1
    fi
    DEVICES_TO_FLASH="$DEVICES_TO_FLASH $DEVICE_ID"
  done
fi

# Print flashing plan
echo -e "Flashing plan:"
echo "  Application: $APP"
echo "  Devices to flash: $DEVICES_TO_FLASH"

set -e

# Build the app
make $APP

BIN_FILE="app/03app_${APP}/Output/nrf52840dk/Debug/Exe/03app_${APP}-nrf52840dk.bin"

if [[ ! -f "$BIN_FILE" ]]; then
  echo "Error: BIN file not found: $BIN_FILE"
  exit 1
fi

# Flash each device
for DEVICE_ID in $DEVICES_TO_FLASH; do
  echo -e "\nFlashing $APP to device with id $DEVICE_ID ..."
  echo "> nrfjprog --snr $DEVICE_ID --program $BIN_FILE --sectorerase --verify --reset"
  nrfjprog --snr "$DEVICE_ID" --program "$BIN_FILE" --sectorerase --verify --reset
done

echo -e "\nAll devices flashed successfully."
