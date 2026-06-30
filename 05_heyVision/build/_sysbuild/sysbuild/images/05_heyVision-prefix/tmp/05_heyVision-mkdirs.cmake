# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/05_heyVision"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/_sysbuild/sysbuild/images/05_heyVision-prefix"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/_sysbuild/sysbuild/images/05_heyVision-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/_sysbuild/sysbuild/images/05_heyVision-prefix/src/05_heyVision-stamp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/_sysbuild/sysbuild/images/05_heyVision-prefix/src"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/_sysbuild/sysbuild/images/05_heyVision-prefix/src/05_heyVision-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/_sysbuild/sysbuild/images/05_heyVision-prefix/src/05_heyVision-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/05_heyVision/build/_sysbuild/sysbuild/images/05_heyVision-prefix/src/05_heyVision-stamp${cfgdir}") # cfgdir has leading slash
endif()
