# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/16_3mic_eval"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/_sysbuild/sysbuild/images/16_3mic_eval-prefix"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/_sysbuild/sysbuild/images/16_3mic_eval-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/_sysbuild/sysbuild/images/16_3mic_eval-prefix/src/16_3mic_eval-stamp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/_sysbuild/sysbuild/images/16_3mic_eval-prefix/src"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/_sysbuild/sysbuild/images/16_3mic_eval-prefix/src/16_3mic_eval-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/_sysbuild/sysbuild/images/16_3mic_eval-prefix/src/16_3mic_eval-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/16_3mic_eval/build/_sysbuild/sysbuild/images/16_3mic_eval-prefix/src/16_3mic_eval-stamp${cfgdir}") # cfgdir has leading slash
endif()
