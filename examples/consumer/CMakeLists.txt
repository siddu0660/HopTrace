# A project that uses HopTrace as a dependency. Either:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<hoptrace install prefix>    (after `cmake --install`)
#   cmake -S . -B build -DHOPTRACE_SOURCE=<path to a HopTrace checkout>  (add_subdirectory / FetchContent style)
cmake_minimum_required(VERSION 3.20)
project(textstats CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(HOPTRACE_SOURCE)
  add_subdirectory(${HOPTRACE_SOURCE} hoptrace)
else()
  find_package(HopTrace REQUIRED)
endif()

# the "codebase under test"
add_library(textstats textstats.cpp)
target_include_directories(textstats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# the benchmark: one header listing the entry points
hoptrace_add_benchmark(textstats_bench
    TARGETS    targets.hpp
    LINK       textstats
    INSTRUMENT textstats)
