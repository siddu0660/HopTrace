/// @file tree.cpp
/// @brief Call trees: naming, merging, recording calls (reservoir sampling), percentiles, complexity fit, tail
/// analysis.
#include "detail.hpp"

#include <algorithm>
#include <unordered_map>

namespace ht {
namespace detail {

void name_tree(ht::Node& n) {
  for (auto& [a, k] : n.kids) {
    k.name = symbol(a);
    name_tree(k);
  }
}

void merge(Node& dst, Node&& src) {
  dst.n += src.n;
  dst.incl_sum += src.incl_sum;
  dst.self_sum += src.self_sum;
  dst.incl_max = std::max(dst.incl_max, src.incl_max);
  dst.timeouts += src.timeouts;
  dst.incl.insert(dst.incl.end(), src.incl.begin(), src.incl.end());
  dst.self.insert(dst.self.end(), src.self.begin(), src.self.end());
  if (dst.incl.size() > Node::kMaxSamples) {
    // shortcut: plain random subset of the pooled samples; slightly over-weights a worker that made fewer calls
    for (size_t i = dst.incl.size() - 1; i > 0; i--) {
      size_t j = rng_next() % (i + 1);
      std::swap(dst.incl[i], dst.incl[j]);
      std::swap(dst.self[i], dst.self[j]);
    }
    dst.incl.resize(Node::kMaxSamples);
    dst.self.resize(Node::kMaxSamples);
  }
  dst.errors += src.errors;
  for (auto& [k, v] : src.counters) dst.counters[k] += v;
  dst.recs_seen += src.recs_seen;
  for (auto& r : src.recs) dst.recs.push_back(std::move(r));
  if (dst.recs.size() > 50000) {  // keep the pooled call sample bounded
    for (size_t i = dst.recs.size() - 1; i > 0; i--) std::swap(dst.recs[i], dst.recs[rng_next() % (i + 1)]);
    dst.recs.resize(50000);
  }
  dst.head.insert(dst.head.end(), src.head.begin(), src.head.end());
  dst.tail.insert(dst.tail.end(), src.tail.begin(), src.tail.end());
  for (auto& [a, k] : src.kids) {
    dst.kids[a].fn = a;
    merge(dst.kids[a], std::move(k));
  }
}

}  // namespace detail

using namespace detail;

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::max<size_t>(1, (size_t)std::ceil(p * v.size())) - 1];
}

void Node::add(double i, double s) {
  n++;
  incl_sum += i;
  self_sum += s;
  incl_max = std::max(incl_max, i);
  if (incl.size() < kMaxSamples) {
    incl.push_back(i);
    self.push_back(s);
    return;
  }
  size_t j = rng_next() % n;  // reservoir sampling (Algorithm R): every call has the same chance to be kept
  if (j < kMaxSamples) {
    incl[j] = i;
    self[j] = s;
  }
}

std::vector<TailHop> tail_diff(const Node& root, size_t top) {
  std::vector<TailHop> out;
  if (root.recs.size() < 100) return out;
  std::vector<const Node::CallRec*> v;
  for (auto& r : root.recs) v.push_back(&r);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->lat < b->lat; });
  size_t n = v.size(), nslow = std::max<size_t>(5, n / 100);
  auto mean_self = [&](size_t from, size_t to) {
    std::unordered_map<uintptr_t, double> m;
    for (size_t i = from; i < to; i++)
      for (auto& [fn, t] : v[i]->self) m[fn] += t;
    for (auto& [fn, t] : m) t /= (double)(to - from);
    return m;
  };
  auto slow = mean_self(n - nslow, n), typ = mean_self(n * 2 / 5, n * 3 / 5);
  for (auto& [fn, t] : slow) out.push_back({symbol(fn), t, typ.count(fn) ? typ[fn] : 0});
  std::sort(out.begin(), out.end(),
            [](auto& a, auto& b) { return a.slow_ns - a.typical_ns > b.slow_ns - b.typical_ns; });
  while (!out.empty() && out.back().slow_ns <= out.back().typical_ns) out.pop_back();
  if (out.size() > top) out.resize(top);
  return out;
}

const char* complexity(const std::vector<std::pair<double, double>>& pts) {
  struct C {
    const char* name;
    double (*f)(double);
  };
  static const C cs[] = {{"O(1)", [](double) { return 1.0; }},
                         {"O(log n)", [](double n) { return std::log2(std::max(n, 2.0)); }},
                         {"O(n)", [](double n) { return n; }},
                         {"O(n log n)", [](double n) { return n * std::log2(std::max(n, 2.0)); }},
                         {"O(n^2)", [](double n) { return n * n; }}};
  const char* best = "?";
  double best_err = INFINITY;
  for (auto& c : cs) {
    // minimise sum(((y - k f) / y)^2) over k  ->  k = sum(f/y) / sum(f^2/y^2)
    double a = 0, b = 0, err = 0;
    for (auto [n, y] : pts)
      if (y > 0) {
        a += c.f(n) / y;
        b += c.f(n) * c.f(n) / (y * y);
      }
    double k = b > 0 ? a / b : 0;
    for (auto [n, y] : pts)
      if (y > 0) err += std::pow((y - k * c.f(n)) / y, 2);
    if (err < best_err) {
      best_err = err;
      best = c.name;
    }
  }
  return best;
}

}  // namespace ht
