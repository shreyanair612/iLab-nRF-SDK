#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(NONE)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
  src/beamform.c
  src/bluetooth.c
  src/vad.c
  src/wakeword.c
  src/nrf_edgeai_generated/nrf_edgeai_user_model.c
)

# NORDIC SDK APP END
