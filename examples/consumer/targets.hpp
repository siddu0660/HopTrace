// Entry points HopTrace calls. Each loop iteration is one timed call; code before the loop is untimed setup.
#pragma once
#include <hoptrace/hoptrace.hpp>
#include "textstats.hpp"

static const char* kWords[] = {"latency", "hop", "trace", "call", "tree", "profile", "p99", "tail"};

static std::string make_text(ht::Ctx& c, size_t words) {
  std::string t;
  for (size_t i = 0; i < words; i++) {
    t += kWords[c.key(8)];
    t += ' ';
  }
  return t;
}

HT_TARGET(most_common, [](ht::Ctx& c) {
  std::string text = make_text(c, c.size);  // --size words, keys from --key-dist
  for (auto _ : c) most_common(text);
});

HT_TARGET(tokenize, [](ht::Ctx& c) {
  std::string text = make_text(c, c.size);
  for (auto _ : c) tokenize(text);
});
