#pragma once
#include <cstdint>
#include <vector>

// A city road network: intersections on a jittered grid, streets to grid neighbours with
// traffic-dependent travel times, plus a few fast "highway" links between distant points.
struct Edge {
  int to;
  double minutes;
};
struct City {
  std::vector<double> x, y;            // intersection coordinates (km)
  std::vector<std::vector<Edge>> adj;  // adjacency list
};

City make_city(int intersections, uint64_t seed);
double distance_km(const City& c, int a, int b);
