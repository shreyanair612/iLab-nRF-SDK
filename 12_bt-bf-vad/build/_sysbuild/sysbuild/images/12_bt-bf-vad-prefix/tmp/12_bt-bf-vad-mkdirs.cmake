# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/12_bt-bf-vad"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/_sysbuild/sysbuild/images/12_bt-bf-vad-prefix"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/_sysbuild/sysbuild/images/12_bt-bf-vad-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/_sysbuild/sysbuild/images/12_bt-bf-vad-prefix/src/12_bt-bf-vad-stamp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/_sysbuild/sysbuild/images/12_bt-bf-vad-prefix/src"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/_sysbuild/sysbuild/images/12_bt-bf-vad-prefix/src/12_bt-bf-vad-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/_sysbuild/sysbuild/images/12_bt-bf-vad-prefix/src/12_bt-bf-vad-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/build/_sysbuild/sysbuild/images/12_bt-bf-vad-prefix/src/12_bt-bf-vad-stamp${cfgdir}") # cfgdir has leading slash
endif()
