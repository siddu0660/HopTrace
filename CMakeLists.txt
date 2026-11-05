cmake_minimum_required(VERSION 3.20)
project(HopTrace VERSION 0.5.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(HT_TOP_LEVEL ON)
else()
  set(HT_TOP_LEVEL OFF)
endif()
option(HOPTRACE_BUILD_TESTS "Build HopTrace's own tests and examples" ${HT_TOP_LEVEL})
option(HOPTRACE_INSTALL "Generate install rules" ${HT_TOP_LEVEL})

if(HT_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
endif()
if(HT_SANITIZE)  # e.g. -DHT_SANITIZE=address,undefined or thread
  add_compile_options(-fsanitize=${HT_SANITIZE})
  add_link_options(-fsanitize=${HT_SANITIZE})
endif()

find_package(Threads REQUIRED)
add_library(hoptrace STATIC
  src/bench_main.cpp src/calib.cpp src/cli.cpp src/clock.cpp src/context.cpp src/instrument.cpp src/main.cpp
  src/report.cpp src/runner.cpp src/saved_run.cpp src/schedule.cpp src/tree.cpp src/util.cpp src/yaml.cpp)
add_library(HopTrace::hoptrace ALIAS hoptrace)
set_source_files_properties(src/calib.cpp PROPERTIES COMPILE_OPTIONS -finstrument-functions)
target_compile_features(hoptrace PUBLIC cxx_std_20)
target_include_directories(hoptrace PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(hoptrace PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
target_link_options(hoptrace PUBLIC -rdynamic)  # lets dladdr name functions in the executable
# run metadata fallback (hoptrace_add_benchmark records the SHA of the project under test instead) and build flags
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE HT_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(TOUPPER "${CMAKE_BUILD_TYPE}" _ht_bt)
target_compile_definitions(hoptrace PRIVATE HT_GIT_SHA="${HT_GIT_SHA}"
  HT_BUILD="${CMAKE_BUILD_TYPE} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_ht_bt}}")

# `hoptrace run|ls|report|diff`
add_executable(hoptrace_cli tools/hoptrace_cli.cpp)
set_target_properties(hoptrace_cli PROPERTIES OUTPUT_NAME hoptrace)
target_link_libraries(hoptrace_cli PRIVATE hoptrace)

include(cmake/hoptrace.cmake)

# API docs (optional): cmake --build build --target docs
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND AND HT_TOP_LEVEL)
  add_custom_target(docs COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMENT "API docs -> build/docs/html/index.html")
endif()

if(HOPTRACE_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(_ht_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/HopTrace)
  install(TARGETS hoptrace EXPORT HopTraceTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(TARGETS hoptrace_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/hoptrace DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})  # all public headers
  install(EXPORT HopTraceTargets NAMESPACE HopTrace:: DESTINATION ${_ht_cmake_dir})
  configure_package_config_file(cmake/HopTraceConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/HopTraceConfig.cmake INSTALL_DESTINATION ${_ht_cmake_dir})
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/HopTraceConfigVersion.cmake COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/HopTraceConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/HopTraceConfigVersion.cmake
                cmake/hoptrace.cmake DESTINATION ${_ht_cmake_dir})
endif()

if(HOPTRACE_BUILD_TESTS)
  add_library(order_book examples/order_book/order_book.cpp)
  hoptrace_add_benchmark(order_book_bench TARGETS examples/order_book/targets.cpp LINK order_book INSTRUMENT order_book)

  enable_testing()
  add_library(synthetic tests/synthetic.cpp)
  hoptrace_instrument(synthetic)
  add_executable(test_tree tests/test_tree.cpp)
  target_link_libraries(test_tree synthetic hoptrace)
  add_test(NAME tree COMMAND test_tree)
  # checks spec parsing + a full mixed run; the SLO is loosened because CI runner speed varies (example says 2ms)
  add_test(NAME spec_file COMMAND order_book_bench --spec=${CMAKE_SOURCE_DIR}/examples/order_book/mixed.yaml --calls=2000 --slo=p99:10s)
  add_test(NAME bad_mix COMMAND order_book_bench --mix=nope:1)
  set_tests_properties(bad_mix PROPERTIES WILL_FAIL TRUE)
  add_test(NAME preset_scaling COMMAND order_book_bench --preset=scaling --sweep=size:100,1000,10000 --calls=100 --warmup=10)
  add_test(NAME preset_stress COMMAND order_book_bench --preset=stress --slo=p99:1s --rate=100000 --calls=200 --warmup=10 --filter=best)
  add_test(NAME preset_cold_trace COMMAND order_book_bench --preset=cold --calls=100 --trace=${CMAKE_BINARY_DIR}/trace.json)
  add_test(NAME bad_preset COMMAND order_book_bench --preset=nope)
  set_tests_properties(bad_preset PROPERTIES WILL_FAIL TRUE)
  add_test(NAME spec_blocks COMMAND order_book_bench --spec=${CMAKE_SOURCE_DIR}/examples/order_book/sweep.yaml --calls=100 --slo=p99:10s)
  add_test(NAME preset_soak COMMAND order_book_bench --preset=soak --duration=1s --rate=20000 --filter=best --timeout=1ms --noise=1 --sample=4 --inject=best_bid:1us)
  add_test(NAME runs_schedule COMMAND order_book_bench --filter=best --calls=500 --runs=3 --pin=compact --schedule=poisson --rate=5000 --key-dist=zipf:1.1 --size-dist=lognormal:1 --json=${CMAKE_BINARY_DIR}/runs.jsonl)
  add_test(NAME bad_schedule COMMAND order_book_bench --schedule=wobble --rate=1)
  set_tests_properties(bad_schedule PROPERTIES WILL_FAIL TRUE)
  # standalone CLI on saved runs
  add_test(NAME cli_save COMMAND order_book_bench --filter=best --calls=2000 --json=${CMAKE_BINARY_DIR}/cli_a.jsonl)
  add_test(NAME cli_report COMMAND hoptrace_cli report ${CMAKE_BINARY_DIR}/cli_a.jsonl --top=3 --folded=${CMAKE_BINARY_DIR}/cli_a.fold)
  add_test(NAME cli_diff COMMAND hoptrace_cli diff ${CMAKE_BINARY_DIR}/cli_a.jsonl ${CMAKE_BINARY_DIR}/cli_a.jsonl)
  set_tests_properties(cli_report cli_diff PROPERTIES DEPENDS cli_save)
  # hoptrace.yaml suites: run twice (second run diffs against the first), list, and reject a bad file
  set(_ht_compose hoptrace_cli run -f ${CMAKE_SOURCE_DIR}/tests/compose.yaml --bench=$<TARGET_FILE:order_book_bench> --out=${CMAKE_BINARY_DIR}/compose)
  add_test(NAME compose_run1 COMMAND ${_ht_compose})
  add_test(NAME compose_run2 COMMAND ${_ht_compose} best)
  set_tests_properties(compose_run2 PROPERTIES DEPENDS compose_run1 PASS_REGULAR_EXPRESSION "best vs previous run")
  add_test(NAME compose_ls COMMAND hoptrace_cli ls -f ${CMAKE_SOURCE_DIR}/tests/compose.yaml)
  set_tests_properties(compose_ls PROPERTIES PASS_REGULAR_EXPRESSION "best .*--warmup=50 --calls=300 --only=order_book_best_bid --slo=p99:10s")
  add_test(NAME compose_bad COMMAND hoptrace_cli run -f ${CMAKE_SOURCE_DIR}/tests/compose.yaml nope)
  set_tests_properties(compose_bad PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli_usage COMMAND hoptrace_cli)
  set_tests_properties(cli_usage PROPERTIES WILL_FAIL TRUE)
endif()
