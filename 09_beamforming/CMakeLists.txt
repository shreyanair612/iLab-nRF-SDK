cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(09_beamforming)

target_sources(app PRIVATE
    src/main.c
    src/wakeword.c
    src/control_output.c
    src/leds.c
)

target_include_directories(app PRIVATE
    src
)