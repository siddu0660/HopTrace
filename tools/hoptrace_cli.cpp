/// @file hoptrace_cli.cpp
/// @brief The `hoptrace` executable: `run` / `ls` suites from hoptrace.yaml, `report` / `diff` saved runs.
#include <hoptrace/hoptrace.hpp>

/// Entry point of the hoptrace tool; see ht::cli().
int main(int argc, char** argv) {
  return ht::cli(argc, argv);
}
