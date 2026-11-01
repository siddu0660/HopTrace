/// @file profile.hpp
/// @brief Running targets programmatically and getting call trees back.
#pragma once

#include <utility>
#include <vector>

#include "node.hpp"
#include "spec.hpp"
#include "target.hpp"

namespace ht {

/// Runs several targets concurrently, as a weighted mix.
///
/// Calls, workers (at least one each) and the arrival rate are split by weight. Measurement starts for all workers
/// at the same moment (after their warmup) and stops together.
/// @param targets (entry point, weight) pairs
/// @param s run options
/// @return one call tree per target, in the same order; the root's `incl` is the end-to-end latency per call
std::vector<Node> profile_mix(const std::vector<std::pair<TargetFn, double>>& targets, const Spec& s);

/// Runs one target. `f` may take `Ctx&` or nothing (see as_target()).
template <class F>
Node profile(F f, const Spec& s) {
  return profile_mix({{as_target(std::move(f)), 1}}, s)[0];
}

/// Runs one target with default options except `calls` and `warmup`.
template <class F>
Node profile(F f, int calls, int warmup) {
  Spec s;
  s.calls = calls;
  s.warmup = warmup;
  return profile(std::move(f), s);
}

/// Measured cost of the instrumentation hooks, subtracted from every node when Spec::calibrate is set.
struct Overhead {
  double leaf_ns;       ///< Hook cost inside a hop's own span.
  double per_child_ns;  ///< Extra time each hop adds to its parent's span.
};

/// @return the hook overhead, measured once (first call) with two tiny instrumented functions
Overhead overhead();

/// @return the clock in use: "tsc <GHz>" when the CPU has an invariant TSC, else "steady_clock (<reason>)"
const char* clock_source();

}  // namespace ht
