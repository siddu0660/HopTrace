/// @file node.hpp
/// @brief The calling-context tree that every measurement ends up in, plus the statistics computed from it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ht {

/// One node of the calling-context tree: a function *as called from one specific chain of callers*.
///
/// The same function reached through two different call paths is two different nodes, so per-path costs stay
/// separate. The root node of a run stands for the target itself (one iteration of the benchmark loop).
///
/// Counts, sums and the maximum are exact. Per-call latencies are kept in `incl` / `self` until there are
/// `kMaxSamples` of them; after that they hold a uniform random sample (reservoir sampling), which keeps memory
/// bounded on long runs while percentiles stay unbiased.
struct Node {
  static constexpr size_t kMaxSamples = 1 << 20;  ///< Per-node cap on stored latency samples.
  static constexpr size_t kWindow = 10000;        ///< Root only: size of the head/tail windows used for drift.

  uintptr_t fn = 0;                        ///< Function address (or a name hash for trees loaded from a file).
  std::string name;                        ///< Demangled function name.
  std::vector<double> incl;                ///< Inclusive latency per call in ns (sampled; pairs with `self` by index).
  std::vector<double> self;                ///< Exclusive latency per call in ns: `incl` minus the children's `incl`.
  size_t n = 0;                            ///< Exact number of calls.
  double incl_sum = 0;                     ///< Exact sum of inclusive time, ns.
  double self_sum = 0;                     ///< Exact sum of exclusive time, ns.
  double incl_max = 0;                     ///< Exact slowest call, ns.
  size_t timeouts = 0;                     ///< Root only: calls slower than Spec::timeout_ns.
  size_t errors = 0;                       ///< Root only: calls the target marked with Ctx::fail().
  std::map<std::string, double> counters;  ///< Root only: totals of Ctx::count().
  std::vector<double> head;                ///< Root only: first kWindow latencies per worker, in time order.
  std::vector<double> tail;                ///< Root only: last kWindow latencies per worker, in time order.

  /// Root only: one whole call, used by tail analysis to compare slow calls with typical ones.
  struct CallRec {
    double lat;                                      ///< End-to-end latency of the call, ns.
    std::vector<std::pair<uintptr_t, double>> self;  ///< Self time per function inside that call, ns.
  };
  std::vector<CallRec> recs;  ///< Root only: a reservoir sample of whole calls.
  size_t recs_seen = 0;       ///< Root only: how many calls were offered to `recs`.

  std::map<uintptr_t, Node> kids;  ///< Callees, keyed by `fn`.

  /// @return the exact number of calls.
  size_t calls() const { return n; }

  /// Records one call: updates the exact totals and reservoir-samples the latencies.
  /// @param incl inclusive time of the call, ns
  /// @param self exclusive time of the call, ns
  void add(double incl, double self);
};

/// @param v samples (copied, then sorted)
/// @param p quantile in [0, 1], e.g. 0.99 for p99
/// @return the nearest-rank percentile, or 0 for an empty vector
double percentile(std::vector<double> v, double p);

/// Fits (n, time) points to O(1), O(log n), O(n), O(n log n) and O(n^2) by least squares on relative error.
/// @return the name of the best-fitting class, e.g. "O(n log n)"
const char* complexity(const std::vector<std::pair<double, double>>& pts);

/// One row of tail analysis: how much more self time a function takes in slow calls than in typical ones.
struct TailHop {
  std::string name;   ///< Function name.
  double slow_ns;     ///< Mean self time per call in the slowest 1% of calls.
  double typical_ns;  ///< Mean self time per call in typical (p40-p60) calls.
};

/// Tail analysis: which functions gain the most self time in the slowest 1% of calls compared with typical ones.
/// Needs at least 100 sampled calls in `root.recs`.
/// @param root a target's root node
/// @param top how many functions to return
std::vector<TailHop> tail_diff(const Node& root, size_t top = 3);

}  // namespace ht
