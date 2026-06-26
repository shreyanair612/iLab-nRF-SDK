# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/04_wakeWord"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/_sysbuild/sysbuild/images/04_wakeWord-prefix"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/_sysbuild/sysbuild/images/04_wakeWord-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/_sysbuild/sysbuild/images/04_wakeWord-prefix/src/04_wakeWord-stamp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/_sysbuild/sysbuild/images/04_wakeWord-prefix/src"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/_sysbuild/sysbuild/images/04_wakeWord-prefix/src/04_wakeWord-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/_sysbuild/sysbuild/images/04_wakeWord-prefix/src/04_wakeWord-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/04_wakeWord/build/_sysbuild/sysbuild/images/04_wakeWord-prefix/src/04_wakeWord-stamp${cfgdir}") # cfgdir has leading slash
endif()
