#!/bin/bash
# One entry point for the test rig.
#
#   tools/rig.sh build      build the firmware (from the Edge AI workspace)
#   tools/rig.sh flash      flash it to the DK over USB
#   tools/rig.sh monitor    open the serial console (logs to sessions/)
#   tools/rig.sh all        build, flash, then monitor
#   tools/rig.sh playlists  generate the 50 wake-word / 50 decoy test audio
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="${WW_EDGEAI_WORKSPACE:-$HOME/Documents/iLAB/nrfedgeAI_workspace}"
TOOLCHAIN="${WW_TOOLCHAIN:-/opt/nordic/ncs/toolchains/0c0f19d91c}"
BOARD="nrf54lm20dk/nrf54lm20b/cpuapp"
BUILD="$PROJECT/build"

export PATH="$PROJECT/tools/bin:$TOOLCHAIN/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$TOOLCHAIN/opt/zephyr-sdk"

build() {
  echo "== building $PROJECT for $BOARD"
  (cd "$WORKSPACE" && west build --build-dir "$BUILD" "$PROJECT" --board "$BOARD" "$@")
}

flash() {
  echo "== looking for the DK"
  local n
  n=$( (nrfutil --json device list 2>/dev/null || true) | (grep -o '"serialNumber":"[^"]*"' || true) | wc -l | tr -d ' ')
  if [ "$n" = "0" ]; then
    echo "No development kit found. Plug the DK's IMCU USB port into this Mac, switch the"
    echo "board POWER switch on, and try again." >&2
    exit 1
  fi
  echo "== flashing"
  (cd "$WORKSPACE" && west flash --build-dir "$BUILD" "$@")
  echo "== done. LED1 lights during a test; LED2 blinks on every detection."
}

monitor() {
  python3 "$PROJECT/tools/monitor.py" "$@"
}

case "${1:-}" in
  build)     shift; build "$@" ;;
  flash)     shift; flash "$@" ;;
  monitor)   shift; monitor "$@" ;;
  all)       shift; build && flash && monitor ;;
  playlists) shift; python3 "$PROJECT/tools/make_playlist.py" "$@" ;;
  *) sed -n '2,9p' "$0"; exit 1 ;;
esac
