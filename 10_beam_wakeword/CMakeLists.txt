cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(10_beam_wakeword)

target_include_directories(app PRIVATE src src/nrf_edgeai_generated)

target_sources(app PRIVATE
  src/main.c
  src/wakeword.c
  src/nrf_edgeai_generated/nrf_edgeai_user_model.c
  src/dmic.c
)