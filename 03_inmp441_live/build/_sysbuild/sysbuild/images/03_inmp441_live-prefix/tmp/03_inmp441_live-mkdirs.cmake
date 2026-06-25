# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/03_inmp441_live"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/_sysbuild/sysbuild/images/03_inmp441_live-prefix"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/_sysbuild/sysbuild/images/03_inmp441_live-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/_sysbuild/sysbuild/images/03_inmp441_live-prefix/src/03_inmp441_live-stamp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/_sysbuild/sysbuild/images/03_inmp441_live-prefix/src"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/_sysbuild/sysbuild/images/03_inmp441_live-prefix/src/03_inmp441_live-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/_sysbuild/sysbuild/images/03_inmp441_live-prefix/src/03_inmp441_live-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/03_inmp441_live/build/_sysbuild/sysbuild/images/03_inmp441_live-prefix/src/03_inmp441_live-stamp${cfgdir}") # cfgdir has leading slash
endif()
