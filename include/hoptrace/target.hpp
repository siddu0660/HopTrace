/// @file target.hpp
/// @brief Declaring benchmark entry points (`HT_TARGET`) and the per-worker context they receive.
#pragma once

#include <functional>
#include <map>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace ht {

/// Handed to a target once per worker thread.
///
/// Code before `for (auto _ : c)` is untimed setup and code after it is untimed teardown; each loop iteration is
/// one timed call. The helpers draw reproducible inputs: the generator is seeded from `--seed` plus the worker index.
///
/// @code
/// HT_TARGET(insert, [](ht::Ctx& c) {
///   auto prices = c.inputs([](std::mt19937_64& r, size_t) { return 90.0 + r() % 2000 / 100.0; });
///   OrderBook ob;                                  // untimed setup
///   for (auto _ : c) ob.insert(prices[c.i % prices.size()]);   // one timed call per iteration
/// });
/// @endcode
class Ctx {
 public:
  size_t size = 1000;   ///< Input size from `--size`.
  int worker = 0;       ///< Worker index, 0 .. workers-1 across the whole run.
  size_t i = 0;         ///< Iteration number on this worker (warmup included), e.g. to index inputs.
  std::mt19937_64 rng;  ///< Seeded from `--seed` + worker, so runs are reproducible.

  /// @return a uniform double in [a, b)
  double uniform(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
  /// @return a normally distributed double
  double normal(double mean, double sd) { return std::normal_distribution<double>(mean, sd)(rng); }
  /// @return a lognormal double with the given *mean* (not log-mean) and log-space sigma
  double lognormal(double mean, double sigma);
  /// @return a rank in 0..n-1 where rank k has weight 1/(k+1)^s (a few ranks are "hot")
  size_t zipf(size_t n, double s);
  /// @return a key index in 0..n-1 drawn from `--key-dist` (uniform | zipf:S)
  size_t key(size_t n);
  /// @return a per-call size drawn from `--size-dist` around `--size`
  size_t draw_size();
  /// @return the lines of `--dataset` (loaded once and shared between workers)
  const std::vector<std::string>& dataset();
  /// Adds `v` to the custom metric `name`; totals are reported per target. Warmup calls aren't counted.
  void count(const std::string& name, double v = 1) { counters[name] += v; }
  /// Marks the current call as an error. It is still timed; errors are counted and checked by `error_rate` SLOs.
  void fail() { failed = true; }

  /// @return `size` inputs, element k produced by `gen(rng, k)`
  template <class F>
  auto inputs(F gen) {
    std::vector<decltype(gen(rng, size_t{}))> v;
    v.reserve(size);
    for (size_t k = 0; k < size; k++) v.push_back(gen(rng, k));
    return v;
  }

  /// Iterator behind `for (auto _ : c)`; every comparison asks the runner whether to do another timed call.
  struct It {
    Ctx* c;                                                 ///< The context being iterated.
    bool operator!=(const It&) const { return c->next(); }  ///< Asks the runner for another timed call.
    void operator++() {}                                    ///< No-op: the runner advances in operator!=.
    int operator*() const { return 0; }                     ///< Unused loop variable.
  };
  It begin() { return {this}; }  ///< Start of the timed loop.
  It end() { return {this}; }    ///< End of the timed loop.

  /// Runner hook: records the previous call (if any), paces the next one, and says whether there is one.
  bool next();

  /// @name Runner state
  /// Set by the runner; targets don't touch these.
  ///@{
  void* rt_ = nullptr;                              ///< Per-worker runner state.
  std::map<std::string, double> counters;           ///< Totals of count().
  bool failed = false;                              ///< Set by fail() for the current call.
  std::string key_dist = "uniform";                 ///< From `--key-dist`.
  std::string size_dist = "fixed";                  ///< From `--size-dist`.
  const std::vector<std::string>* data_ = nullptr;  ///< From `--dataset`.
                                                    ///@}

 private:
  std::vector<double> zcdf_;  // cached zipf CDF for (zn_, zs_)
  size_t zn_ = 0;
  double zs_ = 0;
};

/// A benchmark entry point: called once per worker, runs its own timed loop over the Ctx.
using TargetFn = std::function<void(Ctx&)>;

/// Adapts a callable to a TargetFn. Callables taking `Ctx&` are used as-is; plain `void()` callables become
/// "call it once per iteration".
template <class F>
TargetFn as_target(F f) {
  if constexpr (std::is_invocable_v<F&, Ctx&>)
    return f;
  else
    return [f](Ctx& c) mutable {
      for ([[maybe_unused]] auto _ : c) f();
    };
}

/// A registered entry point.
struct Target {
  std::string name;  ///< Name used by `--list`, `--only`, `--filter` and in reports.
  TargetFn fn;       ///< The entry point.
};

/// @return every target registered with HT_TARGET, in registration order
std::vector<Target>& targets();

/// Registers a target at static-initialisation time; used by HT_TARGET.
struct Reg {
  /// Registers target `n`; see as_target() for what `f` may be.
  template <class F>
  Reg(const char* n, F f) {
    targets().push_back({n, as_target(std::move(f))});
  }
};

}  // namespace ht

/// Declares a benchmark entry point.
/// @param name an identifier; becomes the target name
/// @param ... a callable taking `ht::Ctx&` (setup + `for (auto _ : c)` loop) or no arguments (one call per iteration)
#define HT_TARGET(name, ...) static ::ht::Reg ht_reg_##name(#name, __VA_ARGS__)
