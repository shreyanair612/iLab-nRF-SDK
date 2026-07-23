# Install script for directory: /Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/zephyr

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/opt/nordic/ncs/toolchains/0c0f19d91c/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/arch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/lib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/soc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/boards/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/subsys/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/drivers/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/edge-ai/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/nrf/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/edge-impulse-sdk-zephyr/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/mcuboot/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/mbedtls/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/trusted-firmware-m/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/cjson/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/azure-sdk-for-c/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/cirrus-logic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/openthread/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/memfault-firmware-sdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/canopennode/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/chre/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/cmsis/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/cmsis-dsp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/cmsis-nn/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/cmsis_6/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/fatfs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/hal_nordic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/hal_st/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/hal_tdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/hal_wurthelektronik/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/hostap/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/liblc3/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/libmetal/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/libsbc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/littlefs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/loramac-node/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/lvgl/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/mipi-sys-t/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/nanopb/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/nrf_wifi/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/open-amp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/percepio/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/picolibc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/segger/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/uoscore-uedhoc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/zcbor/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/nrfxlib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/nrf_hw_models/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/modules/connectedhomeip/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/kernel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/cmake/flash/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/cmake/usage/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/cmake/reports/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword/zephyr/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
