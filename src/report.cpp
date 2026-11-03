/// @file report.cpp
/// @brief Output: the terminal call tree, folded stacks, JSON lines and Chrome trace.
#include "detail.hpp"

#include <algorithm>

namespace ht {
namespace detail {

void flatten(const ht::Node& n, std::map<std::string, std::pair<double, size_t>>& m) {
  for (auto& [a, k] : n.kids) {
    auto& e = m[k.name];
    e.first += k.self_sum;
    e.second += k.calls();
    flatten(k, m);
  }
}

void folded(const Node& n, const std::string& path, FILE* f) {  // flamegraph.pl / speedscope input: "a;b;c self_ns"
  for (auto& [a, k] : n.kids) {
    std::string p = path + ";" + k.name;
    fprintf(f, "%s %.0f\n", p.c_str(), k.self_sum);
    folded(k, p, f);
  }
}

/// One JSON object per line: incl = latency sample (ns), n/incl_sum/self_sum/max = exact.
void jsonl(const Node& n, const std::string& target, const std::string& path, FILE* f) {
  fprintf(f, "{\"target\":\"%s\",\"path\":\"%s\",\"n\":%zu,\"incl_sum\":%.0f,\"self_sum\":%.0f,\"max\":%.0f,\"incl\":[",
          esc(target).c_str(), esc(path).c_str(), n.n, n.incl_sum, n.self_sum, n.incl_max);
  for (size_t i = 0; i < n.incl.size(); i++) fprintf(f, "%s%.0f", i ? "," : "", n.incl[i]);
  fprintf(f, "]");
  if (path == target) fprintf(f, ",\"errors\":%zu,\"timeouts\":%zu", n.errors, n.timeouts);
  fprintf(f, "}\n");
  for (auto& [a, k] : n.kids) jsonl(k, target, path + ";" + k.name, f);
}

/// {"tail":"<target>","hops":[["name",slow_ns,typical_ns],...]}
void jsonl_tail(const std::string& target, const std::vector<TailHop>& tail, FILE* f) {
  fprintf(f, "{\"tail\":\"%s\",\"hops\":[", esc(target).c_str());
  for (size_t i = 0; i < tail.size(); i++)
    fprintf(f, "%s[\"%s\",%.0f,%.0f]", i ? "," : "", esc(tail[i].name).c_str(), tail[i].slow_ns, tail[i].typical_ns);
  fprintf(f, "]}\n");
}

void write_trace(const std::vector<std::pair<int, std::vector<Ev>>>& tr, FILE* f) {  // Chrome trace / Perfetto JSON
  std::map<uintptr_t, std::string> names;
  uint64_t t0 = UINT64_MAX;
  for (auto& [g, ev] : tr)
    if (!ev.empty()) t0 = std::min(t0, ev.front().t);
  fprintf(f, "{\"traceEvents\":[");
  bool first = true;
  size_t n = 0;
  for (auto& [g, ev] : tr)
    for (auto& e : ev) {
      if (++n > 2'000'000)
        break;  // shortcut: cap file size (~200 MB); add sampling or a time window if longer traces are needed
      auto it = names.find(e.fn);
      if (it == names.end()) it = names.emplace(e.fn, esc(symbol(e.fn))).first;
      fprintf(f, "%s{\"name\":\"%s\",\"ph\":\"%c\",\"ts\":%.3f,\"pid\":1,\"tid\":%d}", first ? "" : ",\n",
              it->second.c_str(), e.in ? 'B' : 'E', (double)(e.t - t0) / 1e3, g);
      first = false;
    }
  fprintf(f, "]}\n");
}

}  // namespace detail

using namespace detail;

namespace {

/// Prints the tree below `n` as indented rows, children sorted by total time.
void dump(const ht::Node& n, int depth) {
  std::vector<const ht::Node*> ks;
  for (auto& [a, k] : n.kids) ks.push_back(&k);
  std::sort(ks.begin(), ks.end(), [](auto* a, auto* b) { return a->incl_sum > b->incl_sum; });
  for (auto* k : ks) {
    printf("%*s%-*s %8zu %9.2f %9.2f %9.2f %9.2f\n", depth * 2, "", 44 - depth * 2,
           k->name.substr(0, 44 - depth * 2).c_str(), k->calls(), ht::percentile(k->incl, .5) / 1e3,
           ht::percentile(k->incl, .99) / 1e3, k->incl_max / 1e3, k->self_sum / k->calls() / 1e3);
    dump(*k, depth + 1);
  }
}

}  // namespace

void print(const Node& root, int top) {
  printf("%-44s %8s %9s %9s %9s %9s\n", "call tree (us)", "calls", "p50", "p99", "max", "self");
  printf("%-44s %8zu %9.2f %9.2f %9.2f\n", "<target>", root.calls(), percentile(root.incl, .5) / 1e3,
         percentile(root.incl, .99) / 1e3, root.incl_max / 1e3);
  dump(root, 1);

  std::map<std::string, std::pair<double, size_t>> m;
  flatten(root, m);
  std::vector<std::pair<std::string, std::pair<double, size_t>>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
  double total = root.incl_sum;
  printf("\nhot functions by self time:\n");
  for (int i = 0; i < top && i < (int)v.size(); i++)
    printf("  %5.1f%%  %s  (%zu calls)\n", 100 * v[i].second.first / total, v[i].first.c_str(), v[i].second.second);

  printf("\nhot path:");
  for (const Node* n = &root;;) {
    const Node* best = nullptr;
    for (auto& [a, k] : n->kids)
      if (!best || k.incl_sum > best->incl_sum) best = &k;
    if (!best) break;
    printf(" -> %s", best->name.c_str());
    n = best;
  }
  printf("\n");
}

}  // namespace ht
