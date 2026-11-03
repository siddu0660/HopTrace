/// @file runner.cpp
/// @brief The runner: worker threads, warmup/measure barriers, pacing, overhead calibration and profile_mix().
#include "detail.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <fstream>
#include <random>
#include <thread>

namespace ht {
namespace detail {

/// --trace: raw per-thread events kept for the Chrome/Perfetto timeline
std::mutex trace_mu;
std::vector<std::pair<int, std::vector<Ev>>>* trace_out = nullptr;

}  // namespace detail

using namespace detail;

namespace {

/// Streams through 32 MB so the next call starts with cold caches (--cold).
void flush_caches() {
  thread_local std::vector<char> junk(32 << 20);
  for (size_t k = 0; k < junk.size(); k += 64) junk[k]++;
}

/// Completion step of the "start measuring" barrier: picks a common start time and turns recording on.
struct GoFn {
  uint64_t* start;
  void operator()() noexcept {
    *start = now() + 2'000'000;
    recording = true;
  }
};
/// Completion step of the "stop measuring" barrier: turns recording off.
struct DoneFn {
  void operator()() noexcept { recording = false; }
};
/// One worker's runner state, reached through Ctx::rt_ from Ctx::next().
struct Rt {
  int warmup, n, w, idx;
  const Sched* sched;
  uint64_t* start;
  std::barrier<GoFn>* go;
  std::barrier<DoneFn>* done;
  Node* root;
  Replayer* rp;
  int sample = 1;
  uint64_t duration_ns = 0;
  double timeout_ns = 0;
  std::vector<double> tail;  // ring of the last kWindow latencies
  size_t tail_pos = 0;
  double think_ns = 0;
  double hop_cost = 0;  // ns each recorded hop adds to the call (calibrated per_child), subtracted end to end
  size_t ev0 = 0;       // evs.size() when the current call started
  double pnext = 0;     // poisson: offset of this worker's next arrival
  std::mt19937_64 prng;
  int j = 0, m = 0;
  uint64_t t0 = 0;
  bool started = false, finished = false, cold = false;
};
/// One target of a (possibly mixed) run: its entry point, share of calls and workers, and arrival schedule.
struct Job {
  TargetFn fn;
  int calls, workers;
  Sched sched;
};

/// @return the lines of a --dataset file, read once per process and shared by every worker
const std::vector<std::string>* load_dataset(const std::string& path) {
  static std::mutex mu;
  static std::map<std::string, std::vector<std::string>> cache;
  std::lock_guard l(mu);
  auto it = cache.find(path);
  if (it != cache.end()) return &it->second;
  std::ifstream f(path);
  if (!f) {
    fprintf(stderr, "can't read --dataset %s\n", path.c_str());
    exit(2);
  }
  std::vector<std::string> lines;
  for (std::string ln; std::getline(f, ln);) lines.push_back(ln);
  return &cache.emplace(path, std::move(lines)).first->second;
}

/// Runs the jobs on their worker threads and returns one merged call tree per job.
/// Every worker warms up, waits at a barrier, measures until its call or time budget is spent, waits again, then
/// folds its events into its own tree; the trees are merged at the end.
/// @param s0 hook cost inside a hop's own span (subtracted), ns
/// @param p hook cost a hop adds to its parent (subtracted), ns
std::vector<Node> run(const std::vector<Job>& jobs, const Spec& s, double s0, double p) {
  init_clock();
  std::atomic<bool> stop_noise{false};
  std::vector<std::thread> noise;
  for (int i = 0; i < s.noise; i++)
    noise.emplace_back([&] {
      volatile uint64_t x = 0;
      while (!stop_noise.load(std::memory_order_relaxed)) x = x + 1;
    });
  inject = s.inject.empty() ? nullptr : &s.inject;
  const std::vector<std::string>* data = s.dataset.empty() ? nullptr : load_dataset(s.dataset);
  int total = 0;
  for (auto& j : jobs) total += j.workers;
  uint64_t start = 0;
  std::barrier<GoFn> go(total, GoFn{&start});
  std::barrier<DoneFn> done(total, DoneFn{});
  std::vector<std::vector<Node>> roots(jobs.size());
  std::vector<std::thread> th;
  int g = 0;  // global worker index
  for (size_t k = 0; k < jobs.size(); k++) {
    const Job& job = jobs[k];
    roots[k].resize(job.workers);
    for (int i = 0; i < job.workers; i++, g++)
      th.emplace_back([&, k, i, g] {
        int w = job.workers;
        Replayer rp(roots[k][i], s0, p);
        if (!s.pin.empty()) {
          cpu_set_t set;
          CPU_ZERO(&set);
          CPU_SET(s.pin[g % s.pin.size()], &set);
          pthread_setaffinity_np(pthread_self(), sizeof set, &set);  // best effort: ignored if the core isn't allowed
        }
        Rt rt{s.warmup / w, job.calls / w + (i < job.calls % w), w, i, &job.sched, &start, &go, &done, &roots[k][i],
              &rp};
        rt.think_ns = s.think_ns;
        rt.hop_cost = p;
        rt.prng.seed(s.seed * 7919 + g);
        rt.cold = s.cold;
        rt.sample = std::max(1, s.sample);
        rt.duration_ns = (uint64_t)(s.duration_s * 1e9);
        rt.timeout_ns = s.timeout_ns;
        Ctx c;
        c.size = s.size;
        c.worker = g;
        c.rng.seed(s.seed + g);
        c.rt_ = &rt;
        c.key_dist = s.key_dist;
        c.size_dist = s.size_dist;
        c.data_ = data;
        job.fn(c);
        roots[k][i].counters = c.counters;
        // a target that skipped or broke out of its loop must still meet the barriers
        if (!rt.started) {
          evs.clear();
          go.arrive_and_wait();
        }
        if (!rt.finished) done.arrive_and_wait();
        rp.feed(evs);
        if (rt.tail.size() == Node::kWindow)
          std::rotate(rt.tail.begin(), rt.tail.begin() + rt.tail_pos % Node::kWindow, rt.tail.end());
        roots[k][i].tail = std::move(rt.tail);
        if (trace_out) {
          std::lock_guard l(trace_mu);
          trace_out->push_back({g, std::move(evs)});
        }
        evs.clear();
      });
  }
  for (auto& t : th) t.join();
  stop_noise = true;
  for (auto& t : noise) t.join();
  inject = nullptr;
  std::vector<Node> out(jobs.size());
  for (size_t k = 0; k < jobs.size(); k++) {
    for (auto& r : roots[k]) merge(out[k], std::move(r));
    name_tree(out[k]);
  }
  return out;
}

}  // namespace

std::vector<Target>& targets() {
  static std::vector<Target> t;
  return t;
}

bool Ctx::next() {
  Rt& r = *static_cast<Rt*>(rt_);
  if (r.finished) return false;
  if (r.started) {
    double lat = (double)(now() - r.t0);
    // every recorded hop (2 events) inside this call added hook cost to it; unsampled calls fire cheap no-op hooks
    if (sampled) lat = std::max(0.0, lat - r.hop_cost * (double)((evs.size() - r.ev0) / 2));
    r.root->add(lat, lat);
    if (sampled) evs.push_back({0, (uint64_t)lat, false});  // end-of-call marker for tail analysis
    if (failed) {
      r.root->errors++;
      failed = false;
    }
    if (r.timeout_ns > 0 && lat > r.timeout_ns) r.root->timeouts++;
    if (r.root->head.size() < Node::kWindow) r.root->head.push_back(lat);
    if (r.tail.size() < Node::kWindow)
      r.tail.push_back(lat);
    else
      r.tail[r.tail_pos++ % Node::kWindow] = lat;
    r.m++;
  } else if (r.j >= r.warmup) {
    evs.clear();
    counters.clear();  // metrics and failures from warmup calls don't count
    failed = false;
    r.go->arrive_and_wait();
    r.started = true;
  }
  if (r.started && (r.m >= r.n || (r.duration_ns && (int64_t)(now() - *r.start) > (int64_t)r.duration_ns))) {
    r.done->arrive_and_wait();
    r.finished = true;
    return false;
  }
  if (r.started) {
    // between calls: keep memory bounded by folding events into the tree (open loop: time spent here counts as
    // queueing)
    if (evs.size() > (1u << 20) && !trace_out) {
      r.rp->feed(evs);
      evs.clear();
    }
    if (r.cold) flush_caches();
    sampled = r.m % r.sample == 0;
    if (r.sched->kind != Sched::None) {
      double off = r.sched->kind == Sched::Poisson
                       ? (r.pnext += std::exponential_distribution<double>(r.sched->rate / r.w)(r.prng) * 1e9)
                       : r.sched->at((double)((size_t)r.m * r.w + r.idx));
      uint64_t intended = *r.start + (uint64_t)off;
      while (now() < intended) {}  // shortcut: busy-wait pacing, burns a core per worker
      r.t0 = intended;
      r.ev0 = evs.size();
    } else {
      if (r.think_ns > 0 && r.m > 0) {  // closed loop think time (untimed)
        uint64_t end = now() + (uint64_t)r.think_ns;
        if (r.think_ns > 200e3) std::this_thread::sleep_for(std::chrono::nanoseconds((uint64_t)r.think_ns - 100'000));
        while (now() < end) {}
      }
      r.ev0 = evs.size();
      r.t0 = now();
    }
  }
  i = (size_t)r.j++;
  return true;
}

Overhead overhead() {
  static Overhead o = [] {
    Spec s;
    s.calls = 20000;
    s.warmup = 2000;
    Overhead r{0, 0};
    auto med = [&](void (*fn)()) {
      Node n = run({{as_target(fn), s.calls, 1, Sched{}}}, s, 0, 0)[0];
      return percentile(n.kids.begin()->second.incl, .5);
    };
    r.leaf_ns = med(calib_leaf);
    r.per_child_ns = std::max(0.0, med(calib_parent) - r.leaf_ns);
    return r;
  }();
  return o;
}

std::vector<Node> profile_mix(const std::vector<std::pair<TargetFn, double>>& ts, const Spec& s) {
  double sum = 0;
  for (auto& t : ts) sum += t.second;
  std::vector<Job> jobs;
  for (auto& [fn, wt] : ts) {
    double f = sum > 0 ? wt / sum : 1.0 / ts.size();
    jobs.push_back({fn, (int)std::lround(s.calls * f), std::max(1, (int)std::lround(std::max(1, s.workers) * f)),
                    parse_sched(s.schedule, s.rate).scaled(f)});
  }
  Overhead o = s.calibrate ? overhead() : Overhead{0, 0};
  return run(jobs, s, o.leaf_ns, o.per_child_ns);
}

}  // namespace ht
