/// @file detail.hpp
/// @brief Internals shared between HopTrace's source files. Not installed; not part of the public API.
///
/// Module map (src/):
/// - clock.cpp       time source: rdtscp on an invariant TSC, else steady_clock
/// - instrument.cpp  the -finstrument-functions hooks that record enter/exit events, and fault injection
/// - tree.cpp        building call trees from events, merging them, percentiles, complexity fit, tail analysis
/// - schedule.cpp    open-loop arrival schedules (constant, poisson, ramp, step, spike, burst)
/// - runner.cpp      worker threads, warmup/measure barriers, pacing, overhead calibration, profile_mix()
/// - context.cpp     Ctx input helpers (distributions, keys, sizes, dataset)
/// - report.cpp      terminal tree, folded stacks, JSON lines, Chrome trace
/// - saved_run.cpp   reading --json files back, and comparing two runs (diff)
/// - yaml.cpp        the YAML subset behind spec files and hoptrace.yaml
/// - util.cpp        symbols, parsing helpers, small utilities
/// - bench_main.cpp  ht::main(): the benchmark's command line
/// - cli.cpp         ht::cli(): `hoptrace run|ls|report|diff`
///
/// None of these files may be compiled with -finstrument-functions (only calib.cpp is, on purpose).
#pragma once

#include <hoptrace/hoptrace.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define HT_TSC 1
#endif

namespace ht {

/// @name Overhead calibration targets (calib.cpp, compiled with -finstrument-functions)
///@{
void calib_leaf();    ///< Empty instrumented function: measures a hop's own hook cost.
void calib_parent();  ///< Calls calib_leaf(): measures what a child hop adds to its parent.
///@}

/// Internals shared between source files.
namespace detail {

// ---- events (instrument.cpp) ---------------------------------------------------------------------------------------

/// One enter or exit of an instrumented function. A record with `fn == 0` marks the end of a timed call and carries
/// that call's latency in `t` (used by tail analysis).
struct Ev {
  uintptr_t fn;  ///< Function address, or 0 for an end-of-call marker.
  uint64_t t;    ///< Timestamp in ns (or latency for a marker).
  bool in;       ///< true = enter, false = exit.
};

extern thread_local std::vector<Ev> evs;  ///< This thread's events; folded into the tree every ~1M events.
extern thread_local bool sampled;         ///< Whether the current call's hops are recorded (--sample).
extern bool recording;                    ///< True only during the measured phase of a run.
/// Fault injection for the measured phase: function-name substring -> extra ns (nullptr = off).
extern const std::vector<std::pair<std::string, double>>* inject;

/// Spins for the injected delay configured for `fn` (resolved by name once per function per thread).
void inject_delay(uintptr_t fn);

// ---- clock (clock.cpp) ---------------------------------------------------------------------------------------------

extern bool use_tsc;            ///< Set by init_clock() when the TSC is invariant.
extern double ns_per_tick;      ///< TSC calibration against steady_clock.
extern uint64_t tsc0;           ///< TSC value at calibration (timestamps start near 0).
extern std::string clock_desc;  ///< Human-readable clock description (clock_source()).

/// @return steady_clock time in ns
inline uint64_t steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// @return the current time in ns, from the TSC when calibrated, else steady_clock. Hot path: called by every hook.
inline uint64_t now() {
#ifdef HT_TSC
  if (use_tsc) {
    unsigned aux;
    return (uint64_t)((double)(__rdtscp(&aux) - tsc0) * ns_per_tick);
  }
#endif
  return steady_ns();
}

/// Picks and calibrates the clock (once per process). Set HT_CLOCK=steady to force steady_clock.
void init_clock();

// ---- utilities (util.cpp) ------------------------------------------------------------------------------------------

/// @return the demangled name of the function at `fn` (std::string spelled as such), or its hex address
std::string symbol(uintptr_t fn);
/// @return `x` split on commas, empty items dropped
std::vector<std::string> split_csv(const std::string& x);
/// @return `x` escaped for use inside a JSON string
std::string esc(const std::string& x);
/// @return `x` without leading/trailing spaces, tabs and line breaks
std::string trim(const std::string& x);
/// Parses "250us", "5ms", "30s", "2m", "1h" (a bare number is seconds). Exits with status 2 on a bad value.
/// @return seconds
double parse_time(const char* v);
/// Parses "small" (100), "medium" (1e4), "large" (1e6) or a number.
size_t parse_size(const std::string& v);
/// @return the next value of a per-thread xorshift64 generator (reservoir sampling)
uint64_t rng_next();
/// @return this process's resident memory in MB
double rss_mb();
/// @return the sum of `v`
double sum(const std::vector<double>& v);

// ---- arrival schedules (schedule.cpp) ------------------------------------------------------------------------------

// Open-loop arrival schedule: at(k) = ns offset of the job's k-th arrival (all workers together).
struct Sched {
  enum Kind { None, Constant, Poisson, Piecewise, Burst } kind = None;
  double rate = 0;                              // Constant / Poisson: calls per second
  std::vector<std::pair<double, double>> segs;  // Piecewise: (seconds, calls per second); the last one lasts forever
  double bsize = 0, bevery = 0;                 // Burst: calls per burst, seconds between bursts
  double at(double k) const {
    switch (kind) {
      case Constant:
        return k / rate * 1e9;
      case Burst:
        return std::floor(k / bsize) * bevery * 1e9;
      case Piecewise: {
        double t = 0;
        for (size_t i = 0; i < segs.size(); i++) {
          auto [d, r] = segs[i];
          double cnt = r * d;
          if (i + 1 == segs.size() || k < cnt) return (t + (r > 0 ? k / r : 1e9)) * 1e9;
          k -= cnt;
          t += d;
        }
        return t * 1e9;
      }
      default:
        return 0;
    }
  }
  Sched scaled(double f) const {  // share of a mix
    Sched o = *this;
    o.rate *= f;
    for (auto& sg : o.segs) sg.second *= f;
    o.bsize = std::max(1.0, std::round(bsize * f));
    return o;
  }
};

/// Parses Spec::schedule. Exits with status 2 on a bad schedule.
/// @param spec the schedule string ("" = closed loop unless `rate` is set)
/// @param rate Spec::rate, used by constant/poisson
Sched parse_sched(const std::string& spec, double rate);

// ---- call trees (tree.cpp) -----------------------------------------------------------------------------------------

/// Fills in `name` for every node below `n` from its function address.
void name_tree(Node& n);
/// Adds `src`'s calls and children into `dst` (workers' trees become one), keeping sample sizes bounded.
void merge(Node& dst, Node&& src);

// Rebuilds the call tree from one thread's enter/exit events, chunk by chunk. s0/p = hook overhead to subtract (0 =
// raw).
struct Replayer {
  struct Fr {
    Node* n;
    uint64_t t0;
    double kids;
    size_t desc;
  };
  std::vector<Fr> st;
  double s0, p;
  std::vector<std::pair<uintptr_t, double>> cur;  // self time per function within the current call
  Replayer(Node& root, double s0, double p) : st{{&root, 0, 0, 0}}, s0(s0), p(p) {}
  void feed(const std::vector<Ev>& evs) {
    for (auto& e : evs) {
      if (e.fn == 0) {  // end-of-call marker from Ctx::next, t = the call's latency
        Node& root = *st[0].n;
        st.resize(1);  // anything still open was unwound by an exception
        root.recs_seen++;
        Node::CallRec rec{(double)e.t, std::move(cur)};
        cur.clear();
        if (root.recs.size() < 20000)
          root.recs.push_back(std::move(rec));
        else if (size_t j = rng_next() % root.recs_seen; j < 20000)
          root.recs[j] = std::move(rec);
        continue;
      }
      if (e.in) {
        Node& c = st.back().n->kids[e.fn];
        c.fn = e.fn;
        st.push_back({&c, e.t, 0, 0});
      } else {
        // exceptions unwind without exit events: pop frames until the matching one (depth reconciliation)
        size_t i = st.size();
        while (i > 1 && st[i - 1].n->fn != e.fn) i--;
        if (i == 1) continue;  // unmatched exit (e.g. began before recording)
        st.resize(i);
        Fr fr = st.back();
        st.pop_back();
        // a hop's span holds its own hook cost (s0) and the cost of every descendant hop (p each)
        double d = std::max(0.0, (double)(e.t - fr.t0) - s0 - p * fr.desc);
        double self = std::max(0.0, d - fr.kids);
        fr.n->add(d, self);
        auto it = std::find_if(cur.begin(), cur.end(), [&](auto& x) { return x.first == e.fn; });
        if (it == cur.end())
          cur.push_back({e.fn, self});
        else
          it->second += self;
        st.back().kids += d;
        st.back().desc += fr.desc + 1;
      }
    }
  }
};

// ---- Chrome trace capture (runner.cpp) -----------------------------------------------------------------------------

extern std::mutex trace_mu;  ///< Guards trace_out.
/// When set (--trace), each worker hands its raw events here at the end of a run: (worker index, events).
extern std::vector<std::pair<int, std::vector<Ev>>>* trace_out;

// ---- reports (report.cpp) ------------------------------------------------------------------------------------------

/// Sums self time and calls per function name over the whole tree below `n`.
void flatten(const Node& n, std::map<std::string, std::pair<double, size_t>>& m);
/// Writes folded stacks ("a;b;c self_ns" per line) for flamegraph.pl / speedscope.
void folded(const Node& n, const std::string& path, FILE* f);
/// Writes the tree below `n` as JSON lines (one object per node).
void jsonl(const Node& n, const std::string& target, const std::string& path, FILE* f);
/// Writes one tail-analysis line: {"tail":"<target>","hops":[[name, slow_ns, typical_ns],...]}.
void jsonl_tail(const std::string& target, const std::vector<TailHop>& tail, FILE* f);
/// Writes captured events as Chrome Trace Event JSON (open in ui.perfetto.dev).
void write_trace(const std::vector<std::pair<int, std::vector<Ev>>>& tr, FILE* f);

// ---- saved runs (saved_run.cpp) ------------------------------------------------------------------------------------

/// Compares two saved runs hop by hop and prints the significant changes.
/// A hop regressed when its p50 rose by more than `threshold` (relative) and `min_delta_ns` (absolute), with a
/// Mann-Whitney U test p < 0.01.
/// @return the number of regressions
/// @throws std::runtime_error if a file can't be read
int diff(const char* base, const char* cur, double threshold, double min_delta_ns = 250);

// ---- YAML subset (yaml.cpp) ----------------------------------------------------------------------------------------

/// One `key: value` line and the lines nested under it.
struct YNode {
  std::string key, value;
  int line = 0;
  std::deque<YNode> kids;  // deque: references stay valid while parsing
};

/// Parses the YAML subset: indentation-nested `key: value` lines, `[a, b]` lists, `#` comments and `2_000_000`
/// digit separators.
/// @throws std::runtime_error with "file:line: reason" on bad input
YNode read_yaml(const std::string& file);
/// Turns a map node into benchmark flags: `key: value` -> --key=value, `key:` -> --key, and the blocks mix / sweep /
/// slo / inject -> --key=a:1,b:2 (sweep joins knobs with ';').
void yaml_flags(const YNode& m, const std::string& file, std::vector<std::string>& out);
/// @return the flags a spec file stands for
std::vector<std::string> spec_args(const char* file);

}  // namespace detail
}  // namespace ht
