# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/09_beamforming"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/_sysbuild/sysbuild/images/09_beamforming-prefix"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/_sysbuild/sysbuild/images/09_beamforming-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/_sysbuild/sysbuild/images/09_beamforming-prefix/src/09_beamforming-stamp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/_sysbuild/sysbuild/images/09_beamforming-prefix/src"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/_sysbuild/sysbuild/images/09_beamforming-prefix/src/09_beamforming-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/_sysbuild/sysbuild/images/09_beamforming-prefix/src/09_beamforming-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/09_beamforming/build/_sysbuild/sysbuild/images/09_beamforming-prefix/src/09_beamforming-stamp${cfgdir}") # cfgdir has leading slash
endif()
