# Install script for directory: /opt/nordic/ncs/v3.3.1/zephyr

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
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/arch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/lib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/soc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/boards/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/subsys/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/drivers/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/nrf/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/mcuboot/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/mbedtls/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/trusted-firmware-m/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/cjson/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/azure-sdk-for-c/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/cirrus-logic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/openthread/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/memfault-firmware-sdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/hostap/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/canopennode/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/chre/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/cmsis/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/cmsis-dsp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/cmsis-nn/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/cmsis_6/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/fatfs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/hal_nordic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/hal_st/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/hal_tdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/hal_wurthelektronik/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/liblc3/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/libmetal/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/libsbc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/littlefs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/loramac-node/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/lvgl/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/mipi-sys-t/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/nanopb/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/nrf_wifi/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/open-amp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/percepio/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/picolibc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/segger/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/uoscore-uedhoc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/zcbor/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/nrfxlib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/nrf_hw_models/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/modules/connectedhomeip/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/kernel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/cmake/flash/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/cmake/usage/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/cmake/reports/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s/zephyr/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
