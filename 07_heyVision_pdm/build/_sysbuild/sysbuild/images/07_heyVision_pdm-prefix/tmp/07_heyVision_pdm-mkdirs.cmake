# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/07_heyVision_pdm"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/_sysbuild/sysbuild/images/07_heyVision_pdm-prefix"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/_sysbuild/sysbuild/images/07_heyVision_pdm-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/_sysbuild/sysbuild/images/07_heyVision_pdm-prefix/src/07_heyVision_pdm-stamp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/_sysbuild/sysbuild/images/07_heyVision_pdm-prefix/src"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/_sysbuild/sysbuild/images/07_heyVision_pdm-prefix/src/07_heyVision_pdm-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/_sysbuild/sysbuild/images/07_heyVision_pdm-prefix/src/07_heyVision_pdm-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/07_heyVision_pdm/build/_sysbuild/sysbuild/images/07_heyVision_pdm-prefix/src/07_heyVision_pdm-stamp${cfgdir}") # cfgdir has leading slash
endif()
