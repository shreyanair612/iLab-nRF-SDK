#
# Wake-word beamforming evaluation firmware.
# Based on 13_finalProto (nRF54LM20 DK, nRF Connect SDK / Zephyr, nRF Edge AI).
#
# Build from the Edge AI workspace, which is the only west topdir that contains
# the edge-ai module this model needs:
#
#   cd /Users/shreybae/Documents/iLAB/nrfedgeAI_workspace
#   west build -d /Users/shreybae/Documents/iLAB/nRF_SDK/14_ww_eval/build \
#        /Users/shreybae/Documents/iLAB/nRF_SDK/14_ww_eval \
#        --board nrf54lm20dk/nrf54lm20b/cpuapp --pristine
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ww_eval)

target_sources(app PRIVATE
  src/main.c
  src/audio_capture_adapter.c
  src/eval_paths.c
  src/beamformer_delay_sum.c
  src/audio_metrics.c
  src/wakeword_eval.c
  src/run_controller.c
  src/telemetry_protocol.c
  src/telemetry_transport.c
  src/command_interface.c
  src/nrf_edgeai_generated/nrf_edgeai_user_model.c
)

target_include_directories(app PRIVATE src)
