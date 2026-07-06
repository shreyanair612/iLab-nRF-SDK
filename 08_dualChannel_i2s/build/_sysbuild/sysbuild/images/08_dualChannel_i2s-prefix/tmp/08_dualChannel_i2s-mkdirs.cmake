# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/08_dualChannel_i2s"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/_sysbuild/sysbuild/images/08_dualChannel_i2s-prefix"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/_sysbuild/sysbuild/images/08_dualChannel_i2s-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/_sysbuild/sysbuild/images/08_dualChannel_i2s-prefix/src/08_dualChannel_i2s-stamp"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/_sysbuild/sysbuild/images/08_dualChannel_i2s-prefix/src"
  "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/_sysbuild/sysbuild/images/08_dualChannel_i2s-prefix/src/08_dualChannel_i2s-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/_sysbuild/sysbuild/images/08_dualChannel_i2s-prefix/src/08_dualChannel_i2s-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nRF_SDK/08_dualChannel_i2s/build/_sysbuild/sysbuild/images/08_dualChannel_i2s-prefix/src/08_dualChannel_i2s-stamp${cfgdir}") # cfgdir has leading slash
endif()
