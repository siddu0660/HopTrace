#include "textstats.hpp"

#include <cctype>

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    if (std::isalnum((unsigned char)c))
      cur += (char)std::tolower((unsigned char)c);
    else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::map<std::string, int> word_counts(const std::string& text) {
  std::map<std::string, int> m;
  for (auto& w : tokenize(text)) m[w]++;
  return m;
}

std::string most_common(const std::string& text) {
  std::string best;
  int n = 0;
  for (auto& [w, c] : word_counts(text))
    if (c > n) {
      n = c;
      best = w;
    }
  return best;
}
