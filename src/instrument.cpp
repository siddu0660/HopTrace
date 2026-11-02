/// @file instrument.cpp
/// @brief The -finstrument-functions hooks (one event per function enter/exit) and fault injection.
#include "detail.hpp"

#include <unordered_map>

namespace ht {
namespace detail {

thread_local std::vector<Ev> evs;  // folded into the tree every ~1M events between calls, so memory stays bounded
thread_local bool sampled = true;  // --sample: is the current call's hop trace recorded?
bool recording = false;
const std::vector<std::pair<std::string, double>>* inject = nullptr;  // set for the measured phase of a run

void inject_delay(uintptr_t fn) {
  thread_local std::unordered_map<uintptr_t, double> cache;  // name match is resolved once per function per thread
  auto it = cache.find(fn);
  if (it == cache.end()) {
    double ns = 0;
    std::string name = symbol(fn);
    for (auto& [sub, d] : *inject)
      if (name.find(sub) != std::string::npos) ns += d;
    it = cache.emplace(fn, ns).first;
  }
  if (it->second > 0) {
    uint64_t end = now() + (uint64_t)it->second;
    while (now() < end) {}
  }
}

}  // namespace detail
}  // namespace ht

using namespace ht::detail;

// Called by every function compiled with -finstrument-functions. They must not be instrumented themselves.
extern "C" {
/// Enter hook: records the call (when sampled) and applies any injected delay inside the hop.
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* f, void*) {
  if (!recording) return;
  if (sampled) evs.push_back({(uintptr_t)f, now(), true});
  if (inject) inject_delay((uintptr_t)f);  // after the enter timestamp, so the delay lands inside this hop
}

/// Exit hook: records the return (when sampled).
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* f, void*) {
  if (recording && sampled) evs.push_back({(uintptr_t)f, now(), false});
}
}
