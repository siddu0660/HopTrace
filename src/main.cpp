/// @file main.cpp
/// @brief Default `main()` for benchmark executables. The linker only pulls it from the static library when the
///        program doesn't define its own `main()`.
#include <hoptrace/hoptrace.hpp>

/// Runs the benchmark's targets; see ht::main().
int main(int argc, char** argv) {
  return ht::main(argc, argv);
}
