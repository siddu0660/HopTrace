#include "city.hpp"

#include <cmath>
#include <random>

double distance_km(const City& c, int a, int b) {
  return std::hypot(c.x[a] - c.x[b], c.y[a] - c.y[b]);
}

static void connect(City& c, int a, int b, double minutes) {
  c.adj[a].push_back({b, minutes});
  c.adj[b].push_back({a, minutes});
}

City make_city(int intersections, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> jitter(-0.3, 0.3), traffic(1.0, 3.0);
  int side = std::max(2, (int)std::sqrt((double)intersections));
  City c;
  c.adj.resize(side * side);
  for (int r = 0; r < side; r++)
    for (int q = 0; q < side; q++) {
      c.x.push_back(q + jitter(rng));
      c.y.push_back(r + jitter(rng));
    }
  // streets: 2 minutes per km at best, slower with traffic
  for (int r = 0; r < side; r++)
    for (int q = 0; q < side; q++) {
      int v = r * side + q;
      if (q + 1 < side) connect(c, v, v + 1, 2.0 * distance_km(c, v, v + 1) * traffic(rng));
      if (r + 1 < side) connect(c, v, v + side, 2.0 * distance_km(c, v, v + side) * traffic(rng));
    }
  // highways: 1 minute per km, a few long links
  std::uniform_int_distribution<int> any(0, (int)c.adj.size() - 1);
  for (int h = 0; h < side; h++) {
    int a = any(rng), b = any(rng);
    if (a != b) connect(c, a, b, 1.0 * distance_km(c, a, b));
  }
  return c;
}
