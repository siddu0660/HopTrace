/// @file cli.hpp
/// @brief The command-line front ends: the benchmark runner and the `hoptrace` tool.
#pragma once

#include "node.hpp"

namespace ht {

/// Prints a target's call tree, hot functions and hot path in the terminal format.
/// @param root a target's root node
/// @param top how many rows to show under "hot functions"
void print(const Node& root, int top);

/// The benchmark runner behind every executable built with hoptrace_add_benchmark. Run the executable with
/// --help for its options.
/// @return 0 on success, 1 on an SLO breach or regression, 2 on bad arguments
int main(int argc, char** argv);

/// The `hoptrace` tool: `run` / `ls` (suites from hoptrace.yaml), `report` and `diff` (saved runs).
/// @return 0 on success, 1 on an SLO breach or regression, 2 on bad arguments or a failed suite
int cli(int argc, char** argv);

}  // namespace ht
