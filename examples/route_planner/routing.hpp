#pragma once
#include <vector>

#include "city.hpp"

struct Route {
  std::vector<int> path;  // intersections from start to end; empty if unreachable
  double minutes = 0;
  int settled = 0;  // nodes the search finalised (work done)
};

Route dijkstra(const City& city, int from, int to);
Route astar(const City& city, int from, int to);  // euclidean heuristic at highway speed (admissible)
