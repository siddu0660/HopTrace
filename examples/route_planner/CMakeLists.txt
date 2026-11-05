# Sample codebase for trying HopTrace: a ride-routing service (graph search + LRU cache + pricing).
#   cmake -S . -B build -DHOPTRACE_SOURCE=<path to HopTrace>       # or -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake --build build -j && ./build/route_bench --help
cmake_minimum_required(VERSION 3.20)
project(route_planner CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(HOPTRACE_SOURCE)
  add_subdirectory(${HOPTRACE_SOURCE} hoptrace)
else()
  find_package(HopTrace REQUIRED)
endif()

# the codebase under test: nothing in it knows about HopTrace
add_library(route_planner city.cpp routing.cpp service.cpp)
target_include_directories(route_planner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

hoptrace_add_benchmark(route_bench
    TARGETS    targets.hpp
    LINK       route_planner
    INSTRUMENT route_planner)
