/// @file util.cpp
/// @brief Small helpers: symbol names, string parsing, JSON escaping, random numbers, memory usage.
#include "detail.hpp"

#include <cxxabi.h>
#include <dlfcn.h>

#include <cstdlib>
#include <cstring>

namespace ht {
namespace detail {

std::string symbol(uintptr_t fn) {
  Dl_info i;
  if (dladdr((void*)fn, &i) && i.dli_sname) {
    int st = 0;
    char* d = abi::__cxa_demangle(i.dli_sname, nullptr, nullptr, &st);
    std::string s = st == 0 && d ? d : i.dli_sname;
    free(d);
    // readable std types in reports
    for (auto [from, to] :
         {std::pair<const char*, const char*>{
              "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >", "std::string"},
          {"std::basic_string<char, std::char_traits<char>, std::allocator<char> >", "std::string"},
          {"std::__cxx11::", "std::"},
          {"std::__1::", "std::"}})
      for (size_t p; (p = s.find(from)) != std::string::npos;) s.replace(p, strlen(from), to);
    return s;
  }
  char b[32];
  snprintf(b, sizeof b, "0x%lx", (unsigned long)fn);  // static/internal functions aren't in the dynamic symbol table
  return b;
}

std::vector<std::string> split_csv(const std::string& x) {
  std::vector<std::string> out;
  for (size_t i = 0; i <= x.size();) {
    size_t e = x.find(',', i);
    if (e == std::string::npos) e = x.size();
    if (e > i) out.push_back(x.substr(i, e - i));
    i = e + 1;
  }
  return out;
}

std::string esc(const std::string& x) {  // JSON string escape
  std::string o;
  for (char c : x) {
    if (c == '"' || c == '\\') o += '\\';
    if ((unsigned char)c < 0x20) {
      char b[8];
      snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
      continue;
    }
    o += c;
  }
  return o;
}

uint64_t rng_next() {  // xorshift64 for reservoir sampling
  thread_local uint64_t x = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)&x;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

double rss_mb() {
  long pages = 0, res = 0;
  if (FILE* f = fopen("/proc/self/statm", "r")) {
    if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0;
    fclose(f);
  }
  return (double)res * 4096 / 1e6;  // shortcut: assumes 4 KiB pages (true on x86-64 Linux)
}

/// "250us", "5ms", "30s", "2m", "1h"; bare number = seconds. Returns seconds.
double parse_time(const char* v) {
  char* u;
  double x = strtod(v, &u);
  if (!*u || !strcmp(u, "s")) return x;
  if (!strcmp(u, "ns")) return x * 1e-9;
  if (!strcmp(u, "us")) return x * 1e-6;
  if (!strcmp(u, "ms")) return x * 1e-3;
  if (!strcmp(u, "m")) return x * 60;
  if (!strcmp(u, "h")) return x * 3600;
  fprintf(stderr, "bad time '%s' (want e.g. 250us, 5ms, 30s, 2m)\n", v);
  exit(2);
}

double sum(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return s;
}

std::string trim(const std::string& x) {
  size_t a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : x.substr(a, b - a + 1);
}

size_t parse_size(const std::string& v) {
  if (v == "small") return 100;
  if (v == "medium") return 10'000;
  if (v == "large") return 1'000'000;
  return (size_t)atof(v.c_str());
}

}  // namespace detail

using namespace detail;

}  // namespace ht
