/// @file context.cpp
/// @brief Ctx input helpers: distributions, keys, per-call sizes and the shared dataset.
#include "detail.hpp"

#include <algorithm>
#include <cstdlib>

namespace ht {

using namespace detail;

double Ctx::lognormal(double mean, double sigma) {
  return std::lognormal_distribution<double>(std::log(mean) - sigma * sigma / 2, sigma)(rng);
}

size_t Ctx::zipf(size_t n, double s) {
  if (n <= 1) return 0;
  if (n != zn_ || s != zs_) {  // shortcut: O(n) CDF per (n, s), fine up to ~1e7 keys
    zcdf_.resize(n);
    double acc = 0;
    for (size_t k = 0; k < n; k++) zcdf_[k] = acc += 1 / std::pow((double)(k + 1), s);
    for (auto& x : zcdf_) x /= acc;
    zn_ = n;
    zs_ = s;
  }
  double u = std::uniform_real_distribution<double>(0, 1)(rng);
  return std::min<size_t>(n - 1, std::lower_bound(zcdf_.begin(), zcdf_.end(), u) - zcdf_.begin());
}

size_t Ctx::key(size_t n) {
  if (n == 0) return 0;
  if (key_dist.rfind("zipf:", 0) == 0) return zipf(n, atof(key_dist.c_str() + 5));
  return rng() % n;
}

size_t Ctx::draw_size() {
  const std::string& d = size_dist;
  double v = (double)size;
  if (d == "uniform")
    v = uniform(1, 2.0 * size);
  else if (d.rfind("normal:", 0) == 0)
    v = normal((double)size, atof(d.c_str() + 7));
  else if (d.rfind("lognormal:", 0) == 0)
    v = lognormal((double)size, atof(d.c_str() + 10));
  else if (d.rfind("zipf:", 0) == 0)
    v = 1.0 + (double)zipf(size, atof(d.c_str() + 5));
  return (size_t)std::max(1.0, std::round(v));
}

const std::vector<std::string>& Ctx::dataset() {
  static const std::vector<std::string> none;
  return data_ ? *data_ : none;
}

}  // namespace ht
