# Builds the smoke-test mutators against an installed Dyninst.
#
# This goes through find_package rather than a hand-written compiler line
# because the installed public headers include third-party headers of their own
# -- <tbb/concurrent_hash_map.h> among them -- and the include directories that
# resolve those are carried by the exported targets. Listing them by hand means
# rediscovering each one as a CI failure, and on a machine that happens to have
# the library installed system-wide the omission does not show up at all.
#
# Going through the package config also puts the same consumer path
# rocprofiler-systems uses under test, so an install tree that is unusable from
# outside the build directory fails here rather than downstream.
#
# TBB_ROOT_DIR and ElfUtils_ROOT_DIR are read by the find modules the config
# ships; pass the same values the Dyninst build was given.

cmake_minimum_required(VERSION 3.14)

project(dyninst-smoke LANGUAGES CXX)

find_package(Dyninst REQUIRED COMPONENTS dyninstAPI)

foreach(_mutator binary_rewrite runtime_instrument)
  add_executable(${_mutator} "${CMAKE_CURRENT_SOURCE_DIR}/${_mutator}.cpp")
  target_compile_features(${_mutator} PRIVATE cxx_std_17)
  target_link_libraries(${_mutator} PRIVATE Dyninst::dyninstAPI)
endforeach()
