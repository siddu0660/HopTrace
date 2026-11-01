/// @file spec.hpp
/// @brief How a run is shaped: volume, concurrency, arrivals, inputs, faults and measurement options.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ht {

/// Every option of a run. The benchmark's command-line flags, spec files and hoptrace.yaml all fill one of these.
struct Spec {
  /// @name Volume and concurrency
  ///@{
  int calls = 10000;      ///< Timed calls in total, split across workers.
  int warmup = 1000;      ///< Untimed calls first.
  int workers = 1;        ///< Caller threads.
  double duration_s = 0;  ///< Also stop after this long (seconds); whichever of calls/duration comes first.
  std::vector<int> pin;   ///< Cores to pin workers to (cyclic); empty = no pinning.
  ///@}

  /// @name Arrivals
  ///@{

  /// Total calls per second. 0 = closed loop (each call starts when the previous one returns). Above 0 = open loop:
  /// latency counts from each call's *intended* start, so queueing shows up (no coordinated omission).
  double rate = 0;
  /// Open-loop schedule: "" or "constant" (fixed `rate`), "poisson" (exponential gaps at `rate`),
  /// "ramp:FROM:TO:OVER", "step:R1,R2,...:HOLD", "spike:BASE:PEAK:AT:FOR" or "burst:SIZE:EVERY" (times like 10s, 5ms).
  std::string schedule;
  double think_ns = 0;  ///< Closed loop: untimed pause between calls, ns.
  ///@}

  /// @name Inputs
  ///@{
  size_t size = 1000;                ///< Ctx::size.
  uint64_t seed = 1;                 ///< Base seed for Ctx::rng.
  std::string key_dist = "uniform";  ///< Distribution behind Ctx::key(): uniform | zipf:S.
  std::string size_dist =
      "fixed";  ///< Distribution behind Ctx::draw_size(): fixed | uniform | normal:SD | lognormal:SIGMA | zipf:S.
  std::string dataset;  ///< File whose lines Ctx::dataset() returns.
  ///@}

  /// @name Environment and faults
  ///@{
  bool cold = false;  ///< Stream through 32 MB before every timed call (cold caches).
  int noise = 0;      ///< Background CPU-burning threads during the run.
  /// Function-name substring -> extra ns spent inside every matching hop (fault injection).
  std::vector<std::pair<std::string, double>> inject;
  ///@}

  /// @name Measurement
  ///@{
  bool calibrate = true;  ///< Subtract the measured per-hop instrumentation overhead.
  int sample = 1;         ///< Record hops for 1 in N timed calls; end-to-end latency is always recorded.
  double timeout_ns = 0;  ///< Count calls slower than this (Node::timeouts); calls aren't aborted.
  ///@}
};

}  // namespace ht
