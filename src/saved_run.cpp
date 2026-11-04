/// @file saved_run.cpp
/// @brief Reading --json files back (load_run) and comparing two runs hop by hop (diff).
#include "detail.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ht {
namespace detail {

/// Reads a JSON string starting at s[i] == '"'; advances i past the closing quote.
std::string read_jstr(const std::string& s, size_t& i) {
  std::string out;
  for (i++; i < s.size() && s[i] != '"'; i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out += s[i];
      continue;
    }
    char c = s[++i];
    if (c == 'u' && i + 4 < s.size()) {
      out += (char)strtol(s.substr(i + 1, 4).c_str(), nullptr, 16);
      i += 4;
    } else
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
  }
  i++;
  return out;
}

/// One node as saved in a --json file (its `kids` are rebuilt later by trees()).
struct Row {
  std::string target, path;
  Node node;
};  // one saved node; node.kids unused

/// Reads a --json file (throws std::runtime_error on a bad file). `meta`/`tails` get those lines if asked for.
std::vector<Row> load_rows(const char* file, std::string* meta = nullptr, std::vector<std::string>* tails = nullptr) {
  std::ifstream f(file);
  if (!f) throw std::runtime_error(std::string("can't read ") + file);
  std::vector<Row> rows;
  int ln = 0;
  for (std::string line; std::getline(f, line);) {
    ln++;
    if (line.rfind("{\"meta\":", 0) == 0) {
      if (meta) *meta = line;
      continue;
    }
    if (line.rfind("{\"tail\":", 0) == 0) {
      if (tails) tails->push_back(line);
      continue;
    }
    if (line.find("\"target\":") == std::string::npos) continue;
    auto bad = [&](const char* what) {
      return std::runtime_error(std::string(file) + ":" + std::to_string(ln) + ": " + what);
    };
    auto str = [&](const char* k) {
      size_t i = line.find(k);
      if (i == std::string::npos) throw bad(k);
      i += strlen(k) - 1;  // at the opening quote
      return read_jstr(line, i);
    };
    auto num = [&](const char* k, double dflt) {
      size_t i = line.find(k);
      return i == std::string::npos ? dflt : strtod(line.c_str() + i + strlen(k), nullptr);
    };
    Row r{str("\"target\":\""), str("\"path\":\""), {}};
    size_t at = line.find("\"incl\":[");
    if (at == std::string::npos) throw bad("missing incl");
    for (const char* p = line.c_str() + at + 8; *p && *p != ']';) {
      char* e;
      r.node.incl.push_back(strtod(p, &e));
      if (e == p) break;
      p = *e == ',' ? e + 1 : e;
    }
    // files written before these fields existed: fall back to the sample
    r.node.n = (size_t)num("\"n\":", (double)r.node.incl.size());
    r.node.incl_sum = num("\"incl_sum\":", sum(r.node.incl));
    r.node.self_sum = num("\"self_sum\":", 0);
    r.node.incl_max =
        num("\"max\":", r.node.incl.empty() ? 0 : *std::max_element(r.node.incl.begin(), r.node.incl.end()));
    r.node.errors = (size_t)num("\"errors\":", 0);
    r.node.timeouts = (size_t)num("\"timeouts\":", 0);
    size_t semi = r.path.rfind(';');
    r.node.name = semi == std::string::npos ? r.path : r.path.substr(semi + 1);
    rows.push_back(std::move(r));
  }
  return rows;
}

using Samples = std::map<std::string, std::vector<double>>;
Samples load(const char* file) {
  Samples m;
  for (auto& r : load_rows(file)) m[r.target + "|" + r.path] = std::move(r.node.incl);
  return m;
}

/// Rebuild one call tree per target from saved rows (rows are written parent-first).
std::vector<std::pair<std::string, Node>> trees(std::vector<Row>& rows) {
  std::vector<std::pair<std::string, Node>> out;
  out.reserve(rows.size());              // no reallocation: by_path holds pointers into it
  std::map<std::string, Node*> by_path;  // target|path -> node
  for (auto& r : rows) {
    size_t semi = r.path.rfind(';');
    if (semi == std::string::npos) {  // a target's root
      out.push_back({r.target, std::move(r.node)});
      by_path[r.target + "|" + r.path] = &out.back().second;
      continue;
    }
    auto parent = by_path.find(r.target + "|" + r.path.substr(0, semi));
    if (parent == by_path.end()) continue;  // orphan row (truncated file)
    uintptr_t key = std::hash<std::string>{}(r.node.name);
    Node& k = parent->second->kids[key] = std::move(r.node);
    k.fn = key;
    by_path[r.target + "|" + r.path] = &k;
  }
  return out;
}

/// Mann-Whitney U, normal approximation (fine for n >~ 30), two-sided p. Ties get average ranks, no variance
/// correction.
double mwu_p(const std::vector<double>& a, const std::vector<double>& b) {
  std::vector<std::pair<double, int>> all;
  for (double x : a) all.push_back({x, 0});
  for (double x : b) all.push_back({x, 1});
  std::sort(all.begin(), all.end());
  double ra = 0;
  for (size_t i = 0; i < all.size();) {
    size_t j = i;
    while (j < all.size() && all[j].first == all[i].first) j++;
    for (size_t k = i; k < j; k++)
      if (all[k].second == 0) ra += (i + j + 1) / 2.0;  // 1-based average rank
    i = j;
  }
  double na = a.size(), nb = b.size(), u = ra - na * (na + 1) / 2, mu = na * nb / 2,
         sd = std::sqrt(na * nb * (na + nb + 1) / 12);
  return sd > 0 ? std::erfc(std::fabs(u - mu) / sd / std::sqrt(2.0)) : 1;
}

/// A hop regressed when its p50 rose by more than `threshold` (relative) AND `min_delta_ns` (absolute), with p < 0.01.
/// The absolute floor keeps sub-microsecond jitter on tiny functions from failing a run.
int diff(const char* base, const char* cur, double threshold, double min_delta_ns) {
  Samples a = load(base), b = load(cur);
  int bad = 0;
  printf("%-60s %10s %10s %8s %8s\n", "node", "base p50us", "new p50us", "delta", "p");
  for (auto& [k, va] : a) {
    auto it = b.find(k);
    if (it == b.end() || va.size() < 30 || it->second.size() < 30) continue;
    double pa = percentile(va, .5), pb = percentile(it->second, .5), p = mwu_p(va, it->second),
           d = pa > 0 ? (pb - pa) / pa : 0;
    bool big = std::fabs(pb - pa) >= min_delta_ns;
    bool reg = p < 0.01 && d > threshold && big;
    if (reg || (p < 0.01 && d < -threshold && big))
      printf("%-60s %10.2f %10.2f %+7.1f%% %8.1e %s\n", k.substr(0, 60).c_str(), pa / 1e3, pb / 1e3, 100 * d, p,
             reg ? "REGRESSION" : "faster");
    bad += reg;
  }
  printf("%d regression(s)\n", bad);
  return bad ? 1 : 0;
}

}  // namespace detail

using namespace detail;

SavedRun load_run(const char* file) {
  SavedRun run;
  std::string meta;
  std::vector<std::string> tails;
  auto rows = load_rows(file, &meta, &tails);
  // {"meta":{"k":"v",...}}
  for (size_t i = meta.find('{', 1); i != std::string::npos && i < meta.size();) {
    size_t q = meta.find('"', i);
    if (q == std::string::npos) break;
    std::string k = read_jstr(meta, q);
    size_t v = meta.find('"', q);
    if (v == std::string::npos) break;
    run.meta.push_back({k, read_jstr(meta, v)});
    i = v;
  }
  run.targets = trees(rows);
  for (auto& line : tails) {  // {"tail":"t","hops":[["name",slow,typ],...]}
    size_t i = 8;
    std::string target = read_jstr(line, i);
    auto& hops = run.tail[target];
    for (size_t p = line.find("[\"", i); p != std::string::npos; p = line.find("[\"", p)) {
      size_t q = p + 1;
      TailHop h{read_jstr(line, q), 0, 0};
      char* e;
      h.slow_ns = strtod(line.c_str() + q + 1, &e);
      h.typical_ns = strtod(e + 1, nullptr);
      hops.push_back(h);
      p = q;
    }
  }
  return run;
}

}  // namespace ht
