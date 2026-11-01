/// @file clock.cpp
/// @brief Time source: rdtscp calibrated against steady_clock when the TSC is invariant, steady_clock otherwise.
#include "detail.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

namespace ht {
namespace detail {

/// rdtscp when the TSC is invariant (constant + nonstop), calibrated against steady_clock; steady_clock otherwise.
bool use_tsc = false;
double ns_per_tick = 0;
uint64_t tsc0 = 0;
std::string clock_desc = "steady_clock";

void init_clock() {
  static std::once_flag once;
  std::call_once(once, [] {
#ifdef HT_TSC
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    bool constant = false, nonstop = false;
    while (std::getline(f, line))
      if (line.rfind("flags", 0) == 0) {
        constant = line.find(" constant_tsc") != std::string::npos;
        nonstop = line.find(" nonstop_tsc") != std::string::npos;
        break;
      }
    if (getenv("HT_CLOCK") && !strcmp(getenv("HT_CLOCK"), "steady")) {
      clock_desc = "steady_clock (HT_CLOCK=steady)";
      return;
    }
    if (!constant || !nonstop) {
      clock_desc = "steady_clock (TSC not invariant)";
      return;
    }
    unsigned aux;
    uint64_t s0 = steady_ns(), t0 = __rdtscp(&aux);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t s1 = steady_ns(), t1 = __rdtscp(&aux);
    ns_per_tick = (double)(s1 - s0) / (double)(t1 - t0);
    tsc0 = t0;
    use_tsc = true;
    char b[64];
    snprintf(b, sizeof b, "tsc %.2f GHz", 1 / ns_per_tick);
    clock_desc = b;
#endif
  });
}

}  // namespace detail

using namespace detail;

const char* clock_source() {
  init_clock();
  return clock_desc.c_str();
}

}  // namespace ht
