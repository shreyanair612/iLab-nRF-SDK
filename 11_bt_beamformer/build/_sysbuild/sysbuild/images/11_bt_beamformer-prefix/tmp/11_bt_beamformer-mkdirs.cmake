# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/11_bt_beamformer"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/_sysbuild/sysbuild/images/11_bt_beamformer-prefix"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/_sysbuild/sysbuild/images/11_bt_beamformer-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/_sysbuild/sysbuild/images/11_bt_beamformer-prefix/src/11_bt_beamformer-stamp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/_sysbuild/sysbuild/images/11_bt_beamformer-prefix/src"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/_sysbuild/sysbuild/images/11_bt_beamformer-prefix/src/11_bt_beamformer-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/_sysbuild/sysbuild/images/11_bt_beamformer-prefix/src/11_bt_beamformer-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/11_bt_beamformer/build/_sysbuild/sysbuild/images/11_bt_beamformer-prefix/src/11_bt_beamformer-stamp${cfgdir}") # cfgdir has leading slash
endif()
