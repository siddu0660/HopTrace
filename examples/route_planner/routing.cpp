#include "routing.hpp"

#include <algorithm>
#include <limits>
#include <queue>

namespace {
using Item = std::pair<double, int>;  // (priority, node)
}

std::vector<int> rebuild_path(const std::vector<int>& prev, int from, int to) {
  std::vector<int> path;
  for (int v = to; v != -1; v = prev[v]) path.push_back(v);
  std::reverse(path.begin(), path.end());
  return path.front() == from ? path : std::vector<int>{};
}

// lower bound on minutes from v to `to`: straight line at highway speed (1 min/km), so A* stays exact
double straight_line_minutes(const City& city, int v, int to) {
  return 1.0 * distance_km(city, v, to);
}

// shared best-first search; guided = A*, unguided = Dijkstra
Route search(const City& city, int from, int to, bool guided) {
  const double inf = std::numeric_limits<double>::infinity();
  size_t n = city.adj.size();
  std::vector<double> dist(n, inf);
  std::vector<int> prev(n, -1);
  std::vector<char> done(n, 0);
  std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
  Route r;
  dist[from] = 0;
  open.push({guided ? straight_line_minutes(city, from, to) : 0.0, from});
  while (!open.empty()) {
    int v = open.top().second;
    open.pop();
    if (done[v]) continue;
    done[v] = 1;
    r.settled++;
    if (v == to) break;
    for (const Edge& e : city.adj[v])
      if (dist[v] + e.minutes < dist[e.to]) {
        dist[e.to] = dist[v] + e.minutes;
        prev[e.to] = v;
        open.push({dist[e.to] + (guided ? straight_line_minutes(city, e.to, to) : 0.0), e.to});
      }
  }
  if (dist[to] == inf) return r;
  r.minutes = dist[to];
  r.path = rebuild_path(prev, from, to);
  return r;
}

Route dijkstra(const City& city, int from, int to) {
  return search(city, from, to, false);
}

Route astar(const City& city, int from, int to) {
  return search(city, from, to, true);
}
