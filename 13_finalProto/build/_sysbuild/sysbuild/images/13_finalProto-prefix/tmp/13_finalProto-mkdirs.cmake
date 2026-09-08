# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto")
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto")
endif()
file(MAKE_DIRECTORY
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/13_finalProto"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/_sysbuild/sysbuild/images/13_finalProto-prefix"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/_sysbuild/sysbuild/images/13_finalProto-prefix/tmp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/_sysbuild/sysbuild/images/13_finalProto-prefix/src/13_finalProto-stamp"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/_sysbuild/sysbuild/images/13_finalProto-prefix/src"
  "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/_sysbuild/sysbuild/images/13_finalProto-prefix/src/13_finalProto-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/_sysbuild/sysbuild/images/13_finalProto-prefix/src/13_finalProto-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/shreybae/Documents/iLAB/nrfedgeAI_workspace/13_finalProto/build/_sysbuild/sysbuild/images/13_finalProto-prefix/src/13_finalProto-stamp${cfgdir}") # cfgdir has leading slash
endif()
