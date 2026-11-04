/// @file saved_run.hpp
/// @brief Reading back runs saved with `--json`.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "node.hpp"

namespace ht {

/// A run saved with `--json`, loaded back.
struct SavedRun {
  std::vector<std::pair<std::string, std::string>> meta;  ///< Run metadata: cpu, clock, compiler, git, seed, ...
  std::vector<std::pair<std::string, Node>> targets;      ///< One call tree per target; child keys are name hashes.
  std::map<std::string, std::vector<TailHop>> tail;       ///< Tail analysis per target.
};

/// Loads a `--json` file.
/// @throws std::runtime_error if the file can't be read or a line is malformed
SavedRun load_run(const char* jsonl_path);

}  // namespace ht
