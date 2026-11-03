/// @file bench_main.cpp
/// @brief ht::main(): the benchmark command line (flags, spec files, presets, sweeps, stress, reports, SLOs).
#include "detail.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>

#ifndef HT_GIT_SHA
#define HT_GIT_SHA "unknown"  ///< Fallback git SHA: the project HopTrace itself is built in (set by CMake).
#endif
#ifndef HT_BUILD
#define HT_BUILD ""  ///< Build type and flags (set by CMake).
#endif

/// Defined by the source hoptrace_add_benchmark generates: the git SHA of the project under test.
extern "C" __attribute__((weak)) const char* const ht_git_sha;

namespace ht {

using namespace detail;

namespace {

/// "p99:200us,max:5ms" -> checks against the target's end-to-end latency. Returns number of breaches.
/// Checks --slo items ("p99:200us,max:5ms,timeout_rate:0.001,error_rate:0") against a target's root.
/// @return the number of breached items
int check_slo(const std::string& slo, const Node& root, bool verbose = true) {
  int bad = 0;
  for (size_t i = 0; i < slo.size();) {
    size_t e = slo.find(',', i);
    std::string t = slo.substr(i, e == std::string::npos ? e : e - i);
    i = e == std::string::npos ? slo.size() : e + 1;
    size_t c = t.find(':');
    if (c == std::string::npos) {
      fprintf(stderr, "bad --slo item '%s' (want p99:200us)\n", t.c_str());
      exit(2);
    }
    std::string q = t.substr(0, c), lim = t.substr(c + 1);
    if (q == "error_rate") {  // fraction of calls marked with Ctx::fail()
      double got = root.calls() ? (double)root.errors / root.calls() : 0, v = atof(lim.c_str());
      if (verbose) printf("slo error_rate <= %s: got %.4f %s\n", lim.c_str(), got, got <= v ? "ok" : "BREACH");
      bad += got > v;
      continue;
    }
    if (q == "timeout_rate") {  // fraction of calls over --timeout, e.g. timeout_rate:0.001
      double got = root.calls() ? (double)root.timeouts / root.calls() : 0, v = atof(lim.c_str());
      if (verbose) printf("slo timeout_rate <= %s: got %.4f %s\n", lim.c_str(), got, got <= v ? "ok" : "BREACH");
      bad += got > v;
      continue;
    }
    if (q != "max" && (q.size() < 2 || q[0] != 'p')) {
      fprintf(stderr, "bad --slo key '%s' (want pNN, max, timeout_rate or error_rate)\n", q.c_str());
      exit(2);
    }
    char* u;
    double v = strtod(lim.c_str(), &u);
    v *= !strcmp(u, "ns")   ? 1
         : !strcmp(u, "us") ? 1e3
         : !strcmp(u, "ms") ? 1e6
         : !strcmp(u, "s")  ? 1e9
                            : (fprintf(stderr, "bad unit in '%s'\n", t.c_str()), exit(2), 0);
    double got = q == "max" ? root.incl_max : percentile(root.incl, atof(q.c_str() + 1) / 100);
    if (verbose) printf("slo %s <= %s: got %.2fus %s\n", q.c_str(), lim.c_str(), got / 1e3, got <= v ? "ok" : "BREACH");
    bad += got > v;
  }
  return bad;
}

/// Named starting points; anything they set can be overridden by the spec file or CLI.
const std::map<std::string, std::vector<std::string>> presets = {
    {"micro", {"--workers=1", "--rate=0", "--warmup=1000"}},
    {"load", {"--workers=4", "--rate=10000"}},
    {"stress", {"--stress", "--workers=4"}},
    {"concurrency", {"--sweep=workers:1,2,4,8"}},
    {"scaling", {"--sweep=size:100,1000,10000,100000"}},
    {"cold", {"--cold"}},
    {"soak", {"--duration=60s", "--calls=2e9", "--rate=1000", "--workers=2"}},
};

/// @return `x` split on `sep`, empty items dropped
std::vector<std::string> split(const std::string& x, char sep) {
  std::vector<std::string> out;
  for (size_t i = 0; i <= x.size();) {
    size_t e = x.find(sep, i);
    if (e == std::string::npos) e = x.size();
    if (e > i) out.push_back(x.substr(i, e - i));
    i = e + 1;
  }
  return out;
}

using Cell = std::vector<std::pair<std::string, std::string>>;  // knob -> value for one sweep cell

/// @return "k=v,k=v" for a sweep cell, leaving out the knob `skip`
std::string label(const Cell& c, const char* skip = "") {
  std::string l;
  for (auto& [k, v] : c)
    if (k != skip) l += (l.empty() ? "" : ",") + k + "=" + v;
  return l;
}

/// "workers:1,2,4;size:100,1000" -> cartesian product of cells
std::vector<Cell> sweep_cells(const std::string& sw) {
  std::vector<Cell> cells{{}};
  for (auto& part : split(sw, ';')) {
    size_t c = part.find(':');
    std::string k = part.substr(0, c);
    if (c == std::string::npos || (k != "workers" && k != "size" && k != "rate" && k != "calls")) {
      fprintf(stderr, "bad --sweep part '%s' (want workers|size|rate|calls:v1,v2,...)\n", part.c_str());
      exit(2);
    }
    std::vector<Cell> next;
    for (auto& cell : cells)
      for (auto& v : split(part.substr(c + 1), ',')) {
        next.push_back(cell);
        next.back().push_back({k, v});
      }
    cells = next;
  }
  return cells;
}

/// @return `s` with a sweep cell's knob values applied
Spec apply_cell(Spec s, const Cell& c) {
  for (auto& [k, v] : c) {
    if (k == "workers")
      s.workers = atoi(v.c_str());
    else if (k == "size")
      s.size = parse_size(v);
    else if (k == "rate")
      s.rate = atof(v.c_str());
    else if (k == "calls")
      s.calls = (int)atof(v.c_str());
  }
  return s;
}

const char* hottest(const Node& root) {  // function with the most self time
  static std::string best;
  std::map<std::string, std::pair<double, size_t>> m;
  flatten(root, m);
  double t = -1;
  best = "-";
  for (auto& [name, v] : m)
    if (v.first > t) {
      t = v.first;
      best = name;
    }
  return best.c_str();
}

std::string read_first(const char* path,
                       const char* prefix) {  // first line starting with prefix (value after ':' if any)
  std::ifstream f(path);
  for (std::string ln; std::getline(f, ln);)
    if (ln.rfind(prefix, 0) == 0) {
      size_t c = ln.find(':');
      return c == std::string::npos ? ln : trim(ln.substr(c + 1));
    }
  return "";
}

/// @return run metadata (cpu, cores, governor, clock, compiler and flags, git SHA, host, time, seed)
std::vector<std::pair<std::string, std::string>> run_meta(const Spec& s) {
  char host[256] = "";
  gethostname(host, sizeof host - 1);
  char when[32];
  time_t t = time(nullptr);
  strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
  return {{"cpu", read_first("/proc/cpuinfo", "model name")},
          {"cores", std::to_string(std::thread::hardware_concurrency())},
          {"governor", read_first("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "")},
          {"clock", clock_source()},
          {"compiler", __VERSION__},
          {"build", HT_BUILD},
          {"git", &ht_git_sha && ht_git_sha && *ht_git_sha ? ht_git_sha : HT_GIT_SHA},
          {"host", host},
          {"time", when},
          {"seed", std::to_string(s.seed)}};
}

/// p50 and p99 of each repeat, for --runs.
struct RunStats {
  std::vector<double> p50, p99;
};  // per run, for --runs

/// Parses --pin: "compact" (every core in order) or a list of core numbers.
std::vector<int> parse_pin(const std::string& v) {
  std::vector<int> cores;
  if (v == "compact") {
    for (unsigned c = 0; c < std::thread::hardware_concurrency(); c++) cores.push_back((int)c);
  } else
    for (auto& c : split_csv(v)) cores.push_back(atoi(c.c_str()));
  return cores;
}

/// @return whether `d` is a valid --key-dist (size = false) or --size-dist (size = true)
bool valid_dist(const std::string& d, bool size) {
  if (d == "uniform" || d.rfind("zipf:", 0) == 0) return true;
  return size && (d == "fixed" || d.rfind("normal:", 0) == 0 || d.rfind("lognormal:", 0) == 0);
}

}  // namespace

namespace {

/// Everything the benchmark command line can set, after presets, spec files and flags are merged.
struct Options {
  Spec spec;                            ///< Run options.
  int runs = 1;                         ///< --runs: repeats per run (the median run is reported).
  int top = 5;                          ///< --top: rows under "hot functions".
  double threshold = 0.05;              ///< --threshold for --diff.
  double min_delta = 250;               ///< --min-delta for --diff, ns.
  std::vector<std::string> diff_files;  ///< --diff a b: compare two saved runs instead of running.
  bool stress = false;                  ///< --stress: ramp the rate until the SLO breaks.
  std::string filter;                   ///< --filter: substring of target names.
  std::set<std::string> only;           ///< --only: exact target names.
  std::string mix;                      ///< --mix: "a:70,b:30".
  std::string sweep;                    ///< --sweep: "knob:v1,v2;knob:...".
  std::string slo;                      ///< --slo: "p99:200us,error_rate:0".
  std::string json, folded, trace;      ///< Output files.
};

/// Targets run together in one profile_mix() call, with their weights.
using Group = std::vector<std::pair<const Target*, double>>;

/// Open output files for one invocation.
struct Outputs {
  FILE* json = nullptr;                                 ///< --json, or null.
  FILE* folded = nullptr;                               ///< --folded, or null.
  FILE* trace = nullptr;                                ///< --trace, or null.
  std::vector<std::pair<int, std::vector<Ev>>> events;  ///< Captured for --trace.
};

/// Prints the benchmark's usage to `out`.
void usage(FILE* out, const char* prog) {
  fprintf(out,
          "usage: %s [--preset=micro|load|stress|concurrency|scaling|cold|soak] [--spec=file.yaml] [--list]\n"
          "          [--calls=N] [--warmup=N] [--workers=N] [--rate=calls/s] [--size=small|medium|large|N] [--seed=N]\n"
          "          [--only=a,b] [--mix=a:70,b:30] [--sweep=workers:1,2;size:100,1000] [--stress] [--cold] "
          "[--filter=substr] [--top=K]\n"
          "          [--duration=30s] [--timeout=5ms] [--sample=N] [--noise=N] [--inject=fn:50us,...] [--runs=N] "
          "[--pin=compact|0,2]\n"
          "          [--schedule=constant|poisson|ramp:F:T:OVER|step:R1,R2:HOLD|spike:B:P:AT:FOR|burst:N:EVERY] "
          "[--think=1ms]\n"
          "          [--key-dist=uniform|zipf:S] [--size-dist=fixed|uniform|normal:SD|lognormal:SIGMA|zipf:S] "
          "[--dataset=file]\n"
          "          [--slo=p99:200us,max:5ms,timeout_rate:0.001,error_rate:0.01] [--json=f] [--folded=f] [--trace=f] "
          "[--raw]\n"
          "       %s --diff base.json new.json [--threshold=0.05]\n"
          "Docs: docs/reference.md\n",
          prog, prog);
}

/// Merges the three sources of flags in order of precedence: presets < spec files < the command line.
/// @return 0, or 2 on a bad spec file or preset
int collect_args(int argc, char** argv, std::vector<std::string>& args) {
  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "--spec=", 7)) {
      try {
        auto s = spec_args(argv[i] + 7);
        args.insert(args.end(), s.begin(), s.end());
      } catch (const std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 2;
      }
    }
  for (int i = 1; i < argc; i++)
    if (strncmp(argv[i], "--spec=", 7)) args.push_back(argv[i]);
  std::vector<std::string> pre;  // preset expansions go first, so everything else overrides them
  for (auto it = args.begin(); it != args.end();) {
    if (it->rfind("--preset=", 0) != 0) {
      ++it;
      continue;
    }
    auto p = presets.find(it->substr(9));
    if (p == presets.end()) {
      fprintf(stderr, "unknown preset '%s' (have:", it->c_str() + 9);
      for (auto& [k, v] : presets) fprintf(stderr, " %s", k.c_str());
      fprintf(stderr, ")\n");
      return 2;
    }
    pre.insert(pre.end(), p->second.begin(), p->second.end());
    it = args.erase(it);
  }
  args.insert(args.begin(), pre.begin(), pre.end());
  return 0;
}

/// Parses the merged flags into `o`.
/// @return -1 to go on and run, or an exit code: 0 after --help / --list, 2 on a bad flag
int parse_options(const std::vector<std::string>& args, const char* prog, Options& o) {
  Spec& spec = o.spec;
  for (size_t i = 0; i < args.size(); i++) {
    const char* a = args[i].c_str();
    auto val = [&](const char* k) {
      size_t n = strlen(k);
      return strncmp(a, k, n) == 0 ? a + n : nullptr;
    };
    if (auto v = val("--calls="))
      spec.calls = (int)atof(v);  // atof so 1e5 works
    else if (auto v = val("--warmup="))
      spec.warmup = (int)atof(v);
    else if (auto v = val("--workers="))
      spec.workers = atoi(v);
    else if (auto v = val("--rate="))
      spec.rate = atof(v);
    else if (auto v = val("--size="))
      spec.size = parse_size(v);
    else if (auto v = val("--seed="))
      spec.seed = strtoull(v, nullptr, 10);
    else if (auto v = val("--mix="))
      o.mix = v;
    else if (auto v = val("--sweep="))
      o.sweep = v;
    else if (auto v = val("--top="))
      o.top = atoi(v);
    else if (auto v = val("--filter="))
      o.filter = v;
    else if (auto v = val("--only="))
      for (auto& n : split_csv(v)) o.only.insert(n);
    else if (auto v = val("--slo="))
      o.slo = v;
    else if (auto v = val("--json="))
      o.json = v;
    else if (auto v = val("--folded="))
      o.folded = v;
    else if (auto v = val("--trace="))
      o.trace = v;
    else if (auto v = val("--threshold="))
      o.threshold = atof(v);
    else if (auto v = val("--min-delta="))
      o.min_delta = parse_time(v) * 1e9;
    else if (auto v = val("--sample="))
      spec.sample = std::max(1, atoi(v));
    else if (auto v = val("--schedule="))
      spec.schedule = v;
    else if (auto v = val("--think="))
      spec.think_ns = parse_time(v) * 1e9;
    else if (auto v = val("--key-dist="))
      spec.key_dist = v;
    else if (auto v = val("--size-dist="))
      spec.size_dist = v;
    else if (auto v = val("--dataset="))
      spec.dataset = v;
    else if (auto v = val("--pin="))
      spec.pin = parse_pin(v);
    else if (auto v = val("--runs="))
      o.runs = std::max(1, atoi(v));
    else if (auto v = val("--duration="))
      spec.duration_s = parse_time(v);
    else if (auto v = val("--timeout="))
      spec.timeout_ns = parse_time(v) * 1e9;
    else if (auto v = val("--noise="))
      spec.noise = atoi(v);
    else if (auto v = val("--inject=")) {  // name:delay[,name:delay]
      for (auto& item : split(v, ',')) {
        size_t c = item.find(':');
        if (c == std::string::npos || c == 0) {
          fprintf(stderr, "bad --inject item '%s' (want function:delay, e.g. resort:50us)\n", item.c_str());
          return 2;
        }
        spec.inject.push_back({item.substr(0, c), parse_time(item.c_str() + c + 1) * 1e9});
      }
    } else if (!strcmp(a, "--raw"))
      spec.calibrate = false;
    else if (!strcmp(a, "--cold"))
      spec.cold = true;
    else if (!strcmp(a, "--stress"))
      o.stress = true;
    else if (!strcmp(a, "--list")) {
      for (auto& t : targets()) puts(t.name.c_str());
      return 0;
    } else if (!strcmp(a, "--diff") && i + 2 < args.size()) {
      o.diff_files = {args[i + 1], args[i + 2]};
      i += 2;
    } else {
      bool help = !strcmp(a, "--help") || !strcmp(a, "-h");
      if (!help) fprintf(stderr, "unknown or incomplete argument: %s\n", a);
      usage(help ? stdout : stderr, prog);
      return help ? 0 : 2;
    }
  }
  return -1;
}

/// Checks combinations the parser can't see on its own. @return -1 if fine, else 2
int validate(const Options& o) {
  if (!valid_dist(o.spec.key_dist, false)) {
    fprintf(stderr, "bad --key-dist '%s'\n", o.spec.key_dist.c_str());
    return 2;
  }
  if (!valid_dist(o.spec.size_dist, true)) {
    fprintf(stderr, "bad --size-dist '%s'\n", o.spec.size_dist.c_str());
    return 2;
  }
  parse_sched(o.spec.schedule, o.spec.rate);  // exits on a bad schedule
  if (o.stress && o.slo.empty()) {
    fprintf(stderr, "--stress needs --slo (the limit that defines saturation)\n");
    return 2;
  }
  if (o.stress && !o.sweep.empty()) {
    fprintf(stderr, "--stress and --sweep can't be combined\n");
    return 2;
  }
  return -1;
}

/// Decides what to run: one concurrent group for --mix, else one group per selected target.
/// @return -1 if fine, else 2 (unknown target)
int select_groups(const Options& o, std::vector<Group>& groups) {
  if (!o.mix.empty()) {
    groups.emplace_back();
    for (auto& item : split(o.mix, ',')) {
      size_t c = item.find(':');
      std::string name = item.substr(0, c);
      double w = c == std::string::npos ? 1 : atof(item.c_str() + c + 1);
      auto t = std::find_if(targets().begin(), targets().end(), [&](auto& t) { return t.name == name; });
      if (t == targets().end() || w <= 0) {
        fprintf(stderr, "bad --mix item '%s' (unknown target or weight <= 0)\n", item.c_str());
        return 2;
      }
      groups.back().push_back({&*t, w});
    }
    return -1;
  }
  for (auto& t : targets())
    if (t.name.find(o.filter) != std::string::npos && (o.only.empty() || o.only.count(t.name)))
      groups.push_back({{&t, 1}});
  for (auto& n : o.only)
    if (std::none_of(targets().begin(), targets().end(), [&](auto& t) { return t.name == n; })) {
      fprintf(stderr, "--only: no target named '%s'\n", n.c_str());
      return 2;
    }
  return -1;
}

/// Opens --json / --folded / --trace. @return false (after printing why) if one can't be created
bool open_outputs(const Options& o, Outputs& out) {
  out.json = o.json.empty() ? nullptr : fopen(o.json.c_str(), "w");
  out.folded = o.folded.empty() ? nullptr : fopen(o.folded.c_str(), "w");
  out.trace = o.trace.empty() ? nullptr : fopen(o.trace.c_str(), "w");
  if ((!o.json.empty() && !out.json) || (!o.folded.empty() && !out.folded) || (!o.trace.empty() && !out.trace)) {
    perror("output file");
    return false;
  }
  return true;
}

/// Writes captured trace events and closes every output file.
void close_outputs(Outputs& out) {
  if (out.trace) {
    write_trace(out.events, out.trace);
    fclose(out.trace);
    trace_out = nullptr;
  }
  if (out.json) fclose(out.json);
  if (out.folded) fclose(out.folded);
}

/// Prints run metadata and the measured hook overhead, and writes the metadata line of --json.
void print_header(const Options& o, Outputs& out) {
  Overhead ov = o.spec.calibrate ? overhead() : Overhead{0, 0};
  auto meta = run_meta(o.spec);
  printf("run:");
  for (auto& [k, v] : meta)
    if (!v.empty()) printf(" %s=%s%s", k.c_str(), v.c_str(), k == "cores" || k == "clock" ? "\n    " : "");
  printf("\n");
  for (auto& [k, v] : meta)
    if (k == "governor" && !v.empty() && v != "performance")
      printf("tip: CPU governor is '%s'; 'performance' (plus --pin, isolcpus) gives steadier numbers\n", v.c_str());
  if (out.json) {
    fprintf(out.json, "{\"meta\":{");
    for (size_t i = 0; i < meta.size(); i++)
      fprintf(out.json, "%s\"%s\":\"%s\"", i ? "," : "", meta[i].first.c_str(), esc(meta[i].second).c_str());
    fprintf(out.json, "}}\n");
  }
  printf("hook overhead: %.1f ns/hop own span, %.1f ns/hop added to parent%s\n", ov.leaf_ns, ov.per_child_ns,
         o.spec.calibrate ? " (subtracted)" : " (raw, not subtracted)");
  if (out.trace) trace_out = &out.events;  // only after calibration, so its runs aren't traced
}

/// --stress: open-loop ramp that doubles the rate until the SLO breaks; the last passing rate is the saturation point.
void run_stress(const Options& o, const Group& group, const std::vector<std::pair<TargetFn, double>>& fns) {
  printf("\n=== stress ramp:");
  for (auto& [t, w] : group) printf(" %s", t->name.c_str());
  printf(" (slo %s, workers=%d, calls/step=%d) ===\n%12s", o.slo.c_str(), o.spec.workers, o.spec.calls, "rate/s");
  for (auto& [t, w] : group) printf("  %16.16s p50/p99 us", t->name.c_str());
  printf("\n");
  double rate = o.spec.rate > 0 ? o.spec.rate : 1000, ok_rate = 0;
  for (int step = 0; step < 20; step++, rate *= 2) {
    Spec cs = o.spec;
    cs.rate = rate;
    cs.schedule = "";  // the ramp itself is the schedule
    std::vector<Node> roots = profile_mix(fns, cs);
    int breach = 0;
    printf("%12.0f", rate);
    for (auto& r : roots) {
      printf("  %12.2f / %-12.2f", percentile(r.incl, .5) / 1e3, percentile(r.incl, .99) / 1e3);
      breach += check_slo(o.slo, r, false);
    }
    printf("  %s\n", breach ? "BREACH" : "ok");
    if (breach) break;
    ok_rate = rate;
  }
  if (ok_rate > 0)
    printf("saturation: SLO holds up to %.0f calls/s, breaks by %.0f\n", ok_rate, rate);
  else
    printf("saturation: SLO already breached at the starting rate %.0f calls/s (lower --rate)\n", rate);
}

/// @return "runs=N p50 ... p99 ... cv X%" for --runs (empty for a single run); UNSTABLE when cv > 10%
std::string spread(const RunStats& st, int runs) {
  if (runs < 2) return std::string();
  auto& v = st.p50;
  double mean = 0, var = 0;
  for (double x : v) mean += x / v.size();
  for (double x : v) var += (x - mean) * (x - mean) / v.size();
  double cv = mean > 0 ? 100 * std::sqrt(var) / mean : 0;
  char b[200];
  snprintf(b, sizeof b, "runs=%d p50 %.2fus [%.2f..%.2f] p99 %.2fus [%.2f..%.2f] cv %.1f%%%s", runs,
           percentile(v, .5) / 1e3, *std::min_element(v.begin(), v.end()) / 1e3,
           *std::max_element(v.begin(), v.end()) / 1e3, percentile(st.p99, .5) / 1e3,
           *std::min_element(st.p99.begin(), st.p99.end()) / 1e3, *std::max_element(st.p99.begin(), st.p99.end()) / 1e3,
           cv, cv > 10 ? " UNSTABLE" : "");
  return b;
}

/// @return how calls arrive, e.g. "closed-loop", "open-loop 5000/s total", "open-loop poisson @5000/s"
std::string arrival_mode(const Spec& cs) {
  if (!cs.schedule.empty())
    return "open-loop " + cs.schedule +
           (cs.rate > 0 && (cs.schedule == "constant" || cs.schedule == "poisson")
                ? " @" + std::to_string((long)cs.rate) + "/s"
                : "");
  return cs.rate > 0 ? "open-loop " + std::to_string((long)cs.rate) + "/s total" : "closed-loop";
}

/// Prints the full report for one target of a plain (non-sweep) run.
void report_target(const Options& o, const Spec& cs, const std::string& name, const Node& r, size_t group_size,
                   const std::string& spread_line, double rss0, double rss1) {
  printf("\n=== %s (calls=%zu size=%zu workers=%d%s %s%s) ===\n", name.c_str(), r.calls(), cs.size, cs.workers,
         group_size > 1 ? " total, mixed run" : "", arrival_mode(cs).c_str(), cs.cold ? ", cold cache" : "");
  print(r, o.top);
  if (cs.sample > 1) printf("hops recorded for 1 in %d calls (call counts above are sampled)\n", cs.sample);
  if (!spread_line.empty()) printf("%s (tree above is the median run)\n", spread_line.c_str());
  auto tail = tail_diff(r);
  if (!tail.empty()) {
    printf("tail: slowest 1%% of calls vs typical (p40-p60), extra self time per call:\n");
    for (auto& t : tail)
      printf("  %+10.2fus  %s  (%.2fus vs %.2fus)\n", (t.slow_ns - t.typical_ns) / 1e3, t.name.c_str(), t.slow_ns / 1e3,
             t.typical_ns / 1e3);
  }
  if (r.errors) printf("errors: %zu of %zu (%.3f%%)\n", r.errors, r.calls(), 100.0 * r.errors / r.calls());
  for (auto& [cn, cv] : r.counters)
    printf("metric %s: total %.6g, per call %.6g\n", cn.c_str(), cv, r.calls() ? cv / r.calls() : 0);
  if (cs.timeout_ns > 0)
    printf("timeouts (> %.0fus): %zu of %zu (%.3f%%)\n", cs.timeout_ns / 1e3, r.timeouts, r.calls(),
           r.calls() ? 100.0 * r.timeouts / r.calls() : 0);
  if (cs.duration_s > 0 && r.calls() >= 2 * Node::kWindow) {
    double h = percentile(r.head, .5), t = percentile(r.tail, .5);
    printf("drift: p50 of first %zu calls/worker %.2fus -> last %.2fus (%+.1f%%); rss %.1f MB -> %.1f MB\n",
           Node::kWindow, h / 1e3, t / 1e3, h > 0 ? 100 * (t - h) / h : 0, rss0, rss1);
  }
}

/// Runs a group once per sweep cell (once without --sweep), with --runs repeats, and reports each target.
/// @return the number of SLO breaches
int run_cells(const Options& o, const Group& group, const std::vector<std::pair<TargetFn, double>>& fns,
              const std::vector<Cell>& cells, Outputs& out) {
  bool sweeping = !o.sweep.empty();
  int bad = 0;
  std::map<std::string, std::vector<std::pair<double, double>>> fits;  // target [+ other knobs] -> (size, p50)
  if (sweeping)
    printf("\n%-28s %-28s %8s %10s %10s %10s  %s\n", "target", "cell", "calls", "p50 us", "p99 us", "max us",
           "hottest (self)");
  for (auto& cell : cells) {
    Spec cs = apply_cell(o.spec, cell);
    double rss0 = rss_mb();
    // --runs: repeat, keep the run with the median p50 (first target), report the spread across runs
    std::vector<std::vector<Node>> all;
    std::vector<RunStats> stats(group.size());
    for (int r = 0; r < o.runs; r++) {
      all.push_back(profile_mix(fns, cs));
      for (size_t k = 0; k < group.size(); k++) {
        stats[k].p50.push_back(percentile(all.back()[k].incl, .5));
        stats[k].p99.push_back(percentile(all.back()[k].incl, .99));
      }
    }
    std::vector<size_t> order(all.size());
    for (size_t r = 0; r < order.size(); r++) order[r] = r;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return stats[0].p50[a] < stats[0].p50[b]; });
    std::vector<Node> roots = std::move(all[order[order.size() / 2]]);
    all.clear();
    double rss1 = rss_mb();
    for (size_t k = 0; k < group.size(); k++) {
      const std::string& name = group[k].first->name;
      std::string tag = sweeping ? name + "[" + label(cell) + "]" : name;
      if (!sweeping) {
        report_target(o, cs, name, roots[k], group.size(), spread(stats[k], o.runs), rss0, rss1);
      } else {
        printf("%-28.28s %-28.28s %8zu %10.2f %10.2f %10.2f  %.60s\n", name.c_str(), label(cell).c_str(),
               roots[k].calls(), percentile(roots[k].incl, .5) / 1e3, percentile(roots[k].incl, .99) / 1e3,
               roots[k].incl_max / 1e3, hottest(roots[k]));
        if (o.runs > 1) printf("%-58s %s\n", "", spread(stats[k], o.runs).c_str());
        fits[name + "|" + label(cell, "size")].push_back({(double)cs.size, percentile(roots[k].incl, .5)});
      }
      if (out.json) {
        jsonl(roots[k], tag, tag, out.json);
        jsonl_tail(tag, tail_diff(roots[k], 5), out.json);
      }
      if (out.folded) folded(roots[k], tag, out.folded);
      int b = check_slo(o.slo, roots[k], !sweeping);
      if (b && sweeping) printf("%-28s slo breached in %s\n", "", label(cell).c_str());
      bad += b;
    }
  }
  for (auto& [key, pts] : fits)
    if (pts.size() >= 3 && pts.front().first != pts.back().first) {
      size_t bar = key.find('|');
      printf("scaling %s%s%s: p50 fits %s\n", key.substr(0, bar).c_str(), bar + 1 < key.size() ? " @ " : "",
             key.substr(bar + 1).c_str(), complexity(pts));
    }
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  if (int rc = collect_args(argc, argv, args)) return rc;
  Options o;
  if (int rc = parse_options(args, argv[0], o); rc >= 0) return rc;
  if (!o.diff_files.empty()) {  // after parsing, so --threshold / --min-delta anywhere on the line apply
    try {
      return diff(o.diff_files[0].c_str(), o.diff_files[1].c_str(), o.threshold, o.min_delta);
    } catch (const std::exception& e) {
      fprintf(stderr, "%s\n", e.what());
      return 2;
    }
  }
  if (int rc = validate(o); rc >= 0) return rc;
  std::vector<Group> groups;
  if (int rc = select_groups(o, groups); rc >= 0) return rc;
  std::vector<Cell> cells = o.sweep.empty() ? std::vector<Cell>{{}} : sweep_cells(o.sweep);

  Outputs out;
  if (!open_outputs(o, out)) return 2;
  print_header(o, out);
  int bad = 0;
  for (auto& group : groups) {
    std::vector<std::pair<TargetFn, double>> fns;
    for (auto& [t, w] : group) fns.push_back({t->fn, w});
    if (o.stress)
      run_stress(o, group, fns);
    else
      bad += run_cells(o, group, fns, cells, out);
  }
  close_outputs(out);
  return bad ? 1 : 0;
}

}  // namespace ht
