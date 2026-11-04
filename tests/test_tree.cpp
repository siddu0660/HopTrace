#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <hoptrace/hoptrace.hpp>
#include "synthetic.hpp"

#define CHECK(c)                                           \
  do {                                                     \
    if (!(c)) {                                            \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
      exit(1);                                             \
    }                                                      \
  } while (0)

int main() {
  auto root = ht::profile(parent, 50, 5);
  CHECK(root.kids.size() == 1);
  auto& p = root.kids.begin()->second;
  CHECK(p.calls() == 50);
  CHECK(p.kids.size() == 1);
  auto& c = p.kids.begin()->second;
  CHECK(c.calls() == 100);  // 2 per parent call
  double c50 = ht::percentile(c.incl, .5), s50 = ht::percentile(p.self, .5), p50 = ht::percentile(p.incl, .5);
  CHECK(c50 > 195e3 && c50 < 215e3);  // ~200us: bias < ~5% after overhead subtraction
  CHECK(s50 > 45e3 && s50 < 110e3);   // ~50us exclusive time of parent
  CHECK(p50 > 440e3 && p50 < 600e3);  // 50 + 2*200
  // open loop: 2 workers at 1000/s total for 100 calls -> must take ~100ms, latency counted from intended start
  ht::Spec sp;
  sp.calls = 100;
  sp.warmup = 0;
  sp.workers = 2;
  sp.rate = 1000;
  auto t0 = std::chrono::steady_clock::now();
  auto r2 = ht::profile(parent, sp);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms > 90);
  CHECK(r2.kids.begin()->second.calls() == 100);  // merged across both workers
  // exception skips thrower's exit hook; recursion must nest, not merge
  auto er = ht::profile(edge, 50, 5);
  auto& e = er.kids.begin()->second;  // edge() itself
  CHECK(e.kids.size() == 2);          // catcher, recurse
  size_t depth = 0;
  const ht::Node* rec = nullptr;
  for (auto& [a, k] : e.kids)
    if (k.name.find("recurse") == 0) rec = &k;
  CHECK(rec);
  for (auto* n = rec;; n = &n->kids.begin()->second) {
    depth++;
    CHECK(n->calls() == 50);
    if (n->kids.empty()) break;
  }
  CHECK(depth == 4);  // recurse(3..0)
  // Ctx: setup/teardown untimed and unrecorded, loop iterations timed
  auto ct = ht::profile(
      [](ht::Ctx& c) {
        child();  // 200us setup
        for (auto _ : c) recurse(0);
        child();  // teardown
      },
      100, 10);
  CHECK(ct.calls() == 100);
  CHECK(ct.kids.size() == 1 && ct.kids.begin()->second.name.find("recurse") == 0);
  CHECK(ht::percentile(ct.incl, .5) < 50e3);

  // inputs: c.size items, same seed -> same data
  auto first = [](uint64_t seed) {
    double v = 0;
    ht::Spec sp;
    sp.calls = 1;
    sp.warmup = 0;
    sp.size = 7;
    sp.seed = seed;
    ht::profile(
        [&](ht::Ctx& c) {
          auto in = c.inputs([](std::mt19937_64& r, size_t) { return (double)r(); });
          CHECK(in.size() == 7);
          v = in[0];
          for (auto _ : c) {}
        },
        sp);
    return v;
  };
  CHECK(first(5) == first(5) && first(5) != first(6));

  // breaking out early must not deadlock the barriers
  auto br = ht::profile(
      [](ht::Ctx& c) {
        int k = 0;
        for (auto _ : c)
          if (++k == 5) break;
      },
      100, 0);
  CHECK(br.calls() == 4);  // the 5th iteration is cut short by break, so it isn't recorded

  // mix: calls and workers split by weight, run concurrently
  ht::Spec mspec;
  mspec.calls = 400;
  mspec.warmup = 0;
  mspec.workers = 4;
  auto mx = ht::profile_mix({{ht::as_target([] { recurse(1); }), 3}, {ht::as_target([] { recurse(2); }), 1}}, mspec);
  CHECK(mx.size() == 2 && mx[0].calls() == 300 && mx[1].calls() == 100);
  // complexity fit picks the generating curve
  auto pts = [](double (*f)(double)) {
    std::vector<std::pair<double, double>> v;
    for (double n : {100., 1e3, 1e4, 1e5}) v.push_back({n, f(n)});
    return v;
  };
  CHECK(!strcmp(ht::complexity(pts([](double) { return 5.0; })), "O(1)"));
  CHECK(!strcmp(ht::complexity(pts([](double n) { return 2 * n; })), "O(n)"));
  CHECK(!strcmp(ht::complexity(pts([](double n) { return 3 * n * std::log2(n); })), "O(n log n)"));
  CHECK(!strcmp(ht::complexity(pts([](double n) { return n * n; })), "O(n^2)"));
  CHECK(!strcmp(ht::complexity(pts([](double n) { return 7 * std::log2(n); })), "O(log n)"));

  // cold: each timed call pays a cache flush, so it is slower than warm
  ht::Spec cw;
  cw.calls = 200;
  cw.warmup = 10;
  cw.calibrate = false;
  ht::Spec cc = cw;
  cc.cold = true;
  auto touch = [] { recurse(1); };
  CHECK(ht::percentile(ht::profile(touch, cc).incl, .5) >= ht::percentile(ht::profile(touch, cw).incl, .5));
  // reservoir: exact count/sum/max, capped samples, median still right
  {
    ht::Node n;
    const size_t N = 3 * ht::Node::kMaxSamples;
    double expect = 0;
    for (size_t k = 0; k < N; k++) {
      n.add((double)(k % 1000), 0);
      expect += (double)(k % 1000);
    }
    CHECK(n.calls() == N && n.incl.size() == ht::Node::kMaxSamples && n.incl_max == 999);
    CHECK(n.incl_sum == expect);
    double med = ht::percentile(n.incl, .5);
    CHECK(med > 480 && med < 520);
  }

  // sampling: every call timed end to end, hops for 1 in 10
  ht::Spec smp;
  smp.calls = 1000;
  smp.warmup = 0;
  smp.sample = 10;
  auto sm = ht::profile([] { recurse(0); }, smp);
  CHECK(sm.calls() == 1000 && sm.kids.begin()->second.calls() == 100);

  // >1M events: folded into the tree between calls, counts stay exact
  ht::Spec big;
  big.calls = 700000;
  big.warmup = 0;
  big.calibrate = false;
  auto bg = ht::profile([] { recurse(0); }, big);
  CHECK(bg.kids.begin()->second.calls() == 700000);

  // duration stops a run whose call limit is out of reach
  ht::Spec du;
  du.calls = 2'000'000'000;
  du.warmup = 0;
  du.duration_s = 0.2;
  auto d0 = std::chrono::steady_clock::now();
  auto dr = ht::profile([] { recurse(0); }, du);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - d0).count();
  CHECK(secs > 0.19 && secs < 2 && dr.calls() > 0 && dr.calls() < 2'000'000'000u);
  CHECK(!dr.head.empty() && !dr.tail.empty());

  // timeouts: ~200us calls against a 100us limit
  ht::Spec to;
  to.calls = 20;
  to.warmup = 0;
  to.timeout_ns = 100e3;
  CHECK(ht::profile(child, to).timeouts == 20);

  // inject: 100us extra inside recurse(), visible as that hop's latency
  ht::Spec inj;
  inj.calls = 20;
  inj.warmup = 0;
  inj.inject = {{"recurse", 100e3}};
  auto ij = ht::profile([] { recurse(0); }, inj);
  CHECK(ht::percentile(ij.kids.begin()->second.incl, .5) > 95e3);
  // schedules pace arrivals: 200 calls as bursts of 50 every 50ms -> ~150ms; ramp 1000->9000/s over 1s, 3000 calls ->
  // ~0.75s
  auto timed = [](const char* sched, double rate, int calls) {
    ht::Spec sp;
    sp.calls = calls;
    sp.warmup = 0;
    sp.schedule = sched;
    sp.rate = rate;
    sp.calibrate = false;
    auto t0 = std::chrono::steady_clock::now();
    ht::profile([] { recurse(0); }, sp);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  double tb = timed("burst:50:50ms", 0, 200), tr = timed("ramp:1000:9000:1s", 0, 3000),
         tp = timed("poisson", 2000, 1000);
  CHECK(tb > 0.14 && tb < 0.4);
  CHECK(tr > 0.65 && tr < 1.1);
  CHECK(tp > 0.3 && tp < 1.0);  // 1000 calls at 2000/s ~ 0.5s, random gaps

  // tail analysis: 1 call in 50 also runs child() (200us) -> it dominates the slowest 1%
  auto tl = ht::profile(
      [](ht::Ctx& c) {
        for (auto _ : c) {
          recurse(0);
          if (c.i % 50 == 0) child();
        }
      },
      2000, 0);
  auto th = ht::tail_diff(tl);
  CHECK(!th.empty() && th[0].name.find("child") == 0 && th[0].slow_ns > 150e3);

  // errors and custom metrics
  auto em = ht::profile(
      [](ht::Ctx& c) {
        for (auto _ : c) {
          recurse(0);
          c.count("bytes", 10);
          if (c.i % 2) c.fail();
        }
      },
      100, 0);
  CHECK(em.errors == 50 && em.counters.at("bytes") == 1000);

  // distributions
  {
    ht::Ctx c;
    c.rng.seed(3);
    size_t hot = 0, cold = 0;
    double lsum = 0;
    for (int k = 0; k < 20000; k++) {
      size_t z = c.zipf(100, 1.2);
      hot += z == 0;
      cold += z == 99;
      lsum += c.lognormal(500, 1.0);
    }
    CHECK(hot > 20 * (cold + 1));
    CHECK(lsum / 20000 > 450 && lsum / 20000 < 550);
    c.key_dist = "zipf:1.5";
    size_t k0 = 0;
    for (int k = 0; k < 1000; k++) k0 += c.key(1000) == 0;
    CHECK(k0 > 300);
    c.size = 100;
    c.size_dist = "normal:10";
    double ssum = 0;
    for (int k = 0; k < 1000; k++) ssum += (double)c.draw_size();
    CHECK(ssum / 1000 > 95 && ssum / 1000 < 105);
  }

  // dataset + pinning
  {
    FILE* f = fopen("ht_test_dataset.txt", "w");
    fputs("a\nb\nc\n", f);
    fclose(f);
    ht::Spec ds;
    ds.calls = 10;
    ds.warmup = 0;
    ds.dataset = "ht_test_dataset.txt";
    ds.pin = {0};
    size_t lines = 0;
    ht::profile(
        [&](ht::Ctx& c) {
          lines = c.dataset().size();
          for (auto _ : c) {}
        },
        ds);
    CHECK(lines == 3);
    remove("ht_test_dataset.txt");
  }
  // end-to-end latency excludes hook cost: 2000 tiny hops per call barely move the corrected number
  {
    ht::Spec on;
    on.calls = 300;
    on.warmup = 20;
    ht::Spec raw = on;
    raw.calibrate = false;
    auto many = [] {
      for (int k = 0; k < 2000; k++) recurse(0);
    };
    // Best of up to 3 tries: a single p50 on a busy or sanitizer-instrumented machine can drift away from the
    // calibration measured just before it.
    double best = 1e9;
    for (int attempt = 0; attempt < 3 && best >= 0.6; attempt++) {
      double corrected = ht::percentile(ht::profile(many, on).incl, .5),
             uncorrected = ht::percentile(ht::profile(many, raw).incl, .5);
      best = std::min(best, corrected / uncorrected);
    }
    CHECK(best < 0.6);
  }

  // metrics from warmup calls are discarded
  auto wm = ht::profile(
      [](ht::Ctx& c) {
        for (auto _ : c) {
          c.count("n");
          c.fail();
        }
      },
      20, 10);
  CHECK(wm.counters.at("n") == 20 && wm.errors == 20);
  puts("ok");
}
