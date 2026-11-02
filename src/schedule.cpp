/// @file schedule.cpp
/// @brief Open-loop arrival schedules: parsing Spec::schedule into a Sched.
#include "detail.hpp"

#include <cstdlib>

namespace ht {
namespace detail {

Sched parse_sched(const std::string& spec, double rate) {
  Sched sc;
  auto parts = [&] {
    std::vector<std::string> v;
    for (size_t i = 0; i <= spec.size();) {
      size_t e = spec.find(':', i);
      if (e == std::string::npos) e = spec.size();
      v.push_back(spec.substr(i, e - i));
      i = e + 1;
    }
    return v;
  }();
  auto bad = [&] {
    fprintf(stderr,
            "bad --schedule '%s' (want constant | poisson | ramp:FROM:TO:OVER | step:R1,R2:HOLD | "
            "spike:BASE:PEAK:AT:FOR | burst:SIZE:EVERY)\n",
            spec.c_str());
    exit(2);
  };
  const std::string& k = parts[0];
  if (spec.empty() || k == "constant" || k == "poisson") {
    if (rate <= 0) {
      if (spec.empty()) return sc;
      fprintf(stderr, "--schedule=%s needs --rate\n", spec.c_str());
      exit(2);
    }
    sc.kind = k == "poisson" ? Sched::Poisson : Sched::Constant;
    sc.rate = rate;
  } else if (k == "ramp" && parts.size() == 4) {
    double from = atof(parts[1].c_str()), to = atof(parts[2].c_str()), over = parse_time(parts[3].c_str());
    sc.kind = Sched::Piecewise;
    for (int i = 0; i < 100; i++)
      sc.segs.push_back({over / 100, from + (to - from) * (i + 0.5) / 100});  // 100 steps approximate the line
    sc.segs.push_back({0, to});
  } else if (k == "step" && parts.size() == 3) {
    double hold = parse_time(parts[2].c_str());
    sc.kind = Sched::Piecewise;
    for (auto& lv : split_csv(parts[1])) sc.segs.push_back({hold, atof(lv.c_str())});
    if (sc.segs.empty()) bad();
  } else if (k == "spike" && parts.size() == 5) {
    double base = atof(parts[1].c_str()), peak = atof(parts[2].c_str());
    sc.kind = Sched::Piecewise;
    sc.segs = {{parse_time(parts[3].c_str()), base}, {parse_time(parts[4].c_str()), peak}, {0, base}};
  } else if (k == "burst" && parts.size() == 3) {
    sc.kind = Sched::Burst;
    sc.bsize = atof(parts[1].c_str());
    sc.bevery = parse_time(parts[2].c_str());
    if (sc.bsize < 1 || sc.bevery <= 0) bad();
  } else {
    bad();
  }
  for (auto& [d, r] : sc.segs)
    if (r <= 0) bad();
  return sc;
}

}  // namespace detail

using namespace detail;

}  // namespace ht
