# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/10_beam_wakeword"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/_sysbuild/sysbuild/images/10_beam_wakeword-prefix"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/_sysbuild/sysbuild/images/10_beam_wakeword-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/_sysbuild/sysbuild/images/10_beam_wakeword-prefix/src/10_beam_wakeword-stamp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/_sysbuild/sysbuild/images/10_beam_wakeword-prefix/src"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/_sysbuild/sysbuild/images/10_beam_wakeword-prefix/src/10_beam_wakeword-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/_sysbuild/sysbuild/images/10_beam_wakeword-prefix/src/10_beam_wakeword-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/10_beam_wakeword/build/_sysbuild/sysbuild/images/10_beam_wakeword-prefix/src/10_beam_wakeword-stamp${cfgdir}") # cfgdir has leading slash
endif()
