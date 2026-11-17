#!/bin/bash
# One entry point for the 3-mic test rig.
#
#   tools/rig.sh build                 build the firmware (from the Edge AI workspace)
#   tools/rig.sh flash                 flash it to the DK over the IMCU USB port
#   tools/rig.sh monitor               serial console on the device's USB port
#   tools/rig.sh clip [args]           build clips/standard.wav (+ cues) for the speaker
#   tools/rig.sh test --session NAME [--angles 0,30,60,90 --distance 60 ...]
#                                      the hands-free test loop (tools/autotest.py)
#   tools/rig.sh analyze <run_dir>     re-analyse one recording
#   tools/rig.sh report <session_dir>  rebuild a session's comparison report
#   tools/rig.sh verdict <session>...  PASS/FAIL per claim of TEST_PLAN.md, with 95% CIs
#   tools/rig.sh simulate              exercise the host pipeline without hardware
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="${WW_EDGEAI_WORKSPACE:-$HOME/Documents/iLAB/nrfedgeAI_workspace}"
TOOLCHAIN="${WW_TOOLCHAIN:-/opt/nordic/ncs/toolchains/0c0f19d91c}"
BOARD="nrf54lm20dk/nrf54lm20b/cpuapp"
BUILD="$PROJECT/build"

export PATH="$TOOLCHAIN/bin:$TOOLCHAIN/usr/bin:$TOOLCHAIN/opt/bin:$TOOLCHAIN/nrfutil/bin:$TOOLCHAIN/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$TOOLCHAIN/opt/zephyr-sdk"
export ZEPHYR_BASE="$WORKSPACE/zephyr"

build() {
  echo "== building $PROJECT for $BOARD"
  (cd "$WORKSPACE" && west build --build-dir "$BUILD" "$PROJECT" --board "$BOARD" "$@")
}

flash() {
  echo "== flashing (DK IMCU port, POWER on)"
  (cd "$WORKSPACE" && west flash --build-dir "$BUILD" "$@")
  echo "== done. LED2 lights when the laptop has the USB audio port open; LED1 while a test runs."
}

case "${1:-}" in
  build)     shift; build "$@" ;;
  flash)     shift; flash "$@" ;;
  monitor)   shift; python3 "$PROJECT/tools/monitor.py" "$@" ;;
  clip)      shift; python3 "$PROJECT/tools/make_test_clip.py" "$@" ;;
  test)      shift; python3 "$PROJECT/tools/autotest.py" "$@" ;;
  analyze)   shift; python3 "$PROJECT/tools/analyze_run.py" "$@" ;;
  report)    shift; python3 "$PROJECT/tools/session_report.py" "$@" ;;
  verdict)   shift; python3 "$PROJECT/tools/verdict.py" "$@" ;;
  simulate)  shift; python3 "$PROJECT/tools/autotest.py" --simulate --session sim --no-voice "$@" ;;
  *) sed -n '2,14p' "$0"; exit 1 ;;
esac
