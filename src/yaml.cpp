/// @file yaml.cpp
/// @brief The YAML subset behind spec files and hoptrace.yaml.
#include "detail.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace ht {
namespace detail {

// YAML subset used by spec files and hoptrace.yaml: indentation-nested `key: value` lines, `[a, b]` lists, `#`
// comments, `2_000_000` digit separators. shortcut: no flow maps/anchors/multi-docs/quoting; switch to yaml-cpp if
// files outgrow this.

YNode read_yaml(const std::string& file) {
  std::ifstream f(file);
  if (!f) throw std::runtime_error("can't read " + file);
  YNode root;
  std::vector<std::pair<int, YNode*>> stack{{-1, &root}};
  int ln = 0;
  for (std::string line; std::getline(f, line);) {
    ln++;
    line = line.substr(0, line.find('#'));
    if (trim(line).empty()) continue;
    if (line.find('\t') < line.find_first_not_of(" \t"))
      throw std::runtime_error(file + ":" + std::to_string(ln) + ": use spaces, not tabs, to indent");
    int indent = (int)line.find_first_not_of(' ');
    size_t c = line.find(':');
    if (c == std::string::npos) throw std::runtime_error(file + ":" + std::to_string(ln) + ": expected 'key: value'");
    std::string k = trim(line.substr(0, c)), v = trim(line.substr(c + 1));
    if (!v.empty() && v.front() == '[') {  // list -> "a,b,c"
      if (v.back() != ']') throw std::runtime_error(file + ":" + std::to_string(ln) + ": unterminated list");
      v = v.substr(1, v.size() - 2);
      v.erase(std::remove_if(v.begin(), v.end(), [](char ch) { return ch == ' ' || ch == '_'; }), v.end());
    }
    if (!v.empty() && v.find_first_not_of("0123456789_.e") == std::string::npos)
      v.erase(std::remove(v.begin(), v.end(), '_'), v.end());
    while (stack.back().first >= indent) stack.pop_back();
    YNode& parent = *stack.back().second;
    if (!parent.value.empty())
      throw std::runtime_error(file + ":" + std::to_string(ln) + ": '" + parent.key +
                               "' has a value, so it can't have nested keys");
    parent.kids.push_back({k, v, ln, {}});
    stack.push_back({indent, &parent.kids.back()});
  }
  return root;
}

/// Benchmark flags from a map node: `key: value` -> --key=value, `key:` alone -> --key, and the blocks
/// mix / sweep / slo / inject -> --key=a:1,b:2 (sweep joins knobs with ';').
void yaml_flags(const YNode& m, const std::string& file, std::vector<std::string>& out) {
  for (auto& k : m.kids) {
    if (k.kids.empty()) {
      out.push_back("--" + k.key + (k.value.empty() ? "" : "=" + k.value));
      continue;
    }
    if (k.key != "mix" && k.key != "sweep" && k.key != "slo" && k.key != "inject")
      throw std::runtime_error(file + ":" + std::to_string(k.line) +
                               ": only mix, sweep, slo and inject take a nested block, not '" + k.key + "'");
    std::string acc;
    for (auto& item : k.kids) {
      if (!item.kids.empty()) throw std::runtime_error(file + ":" + std::to_string(item.line) + ": too deeply nested");
      acc += (acc.empty() ? "" : k.key == "sweep" ? ";" : ",") + item.key + ":" + item.value;
    }
    out.push_back("--" + k.key + "=" + acc);
  }
}

std::vector<std::string> spec_args(const char* file) {
  std::vector<std::string> out;
  yaml_flags(read_yaml(file), file, out);
  return out;
}

}  // namespace detail

using namespace detail;

}  // namespace ht
