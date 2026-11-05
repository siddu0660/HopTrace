// HopTrace entry points for the route planner. Each `for (auto _ : c)` iteration is one timed call;
// code before the loop is untimed setup (building the city, generating requests).
#pragma once
#include <hoptrace/hoptrace.hpp>

#include <string>
#include <vector>

#include "service.hpp"

// A pool of trips. With --key-dist=zipf:S a few trips are very popular (good for the cache).
static std::vector<std::pair<int, int>> make_trips(ht::Ctx& c, int intersections, size_t n) {
  std::vector<std::pair<int, int>> trips;
  for (size_t k = 0; k < n; k++) trips.push_back({(int)(c.rng() % intersections), (int)(c.rng() % intersections)});
  return trips;
}

// Full request path: parse -> cache -> A*/Dijkstra on a miss -> price -> response.
HT_TARGET(handle_request, [](ht::Ctx& c) {
  int n = (int)c.size;           // --size = intersections in the city
  RouteService svc(n, 42, 256);  // same city on every worker; 256-entry LRU cache
  auto trips = make_trips(c, n, 2000);
  std::vector<std::string> queries;
  for (auto [a, b] : trips)
    queries.push_back("from=" + std::to_string(a) + "&to=" + std::to_string(b) +
                      (c.rng() % 10 ? "&mode=fast" : "&mode=exact"));
  for (auto _ : c) {
    uint64_t hits = svc.hits;
    if (svc.handle(queries[c.key(queries.size())]).empty()) c.fail();  // trip choice follows --key-dist
    c.count("cache_hit", (double)(svc.hits - hits));
  }
});

// The routing algorithms on their own, same trips, no cache.
HT_TARGET(dijkstra, [](ht::Ctx& c) {
  City city = make_city((int)c.size, 42);
  auto trips = make_trips(c, (int)city.adj.size(), 1000);
  for (auto _ : c) {
    auto [a, b] = trips[c.i % trips.size()];
    c.count("settled", dijkstra(city, a, b).settled);
  }
});

HT_TARGET(astar, [](ht::Ctx& c) {
  City city = make_city((int)c.size, 42);
  auto trips = make_trips(c, (int)city.adj.size(), 1000);
  for (auto _ : c) {
    auto [a, b] = trips[c.i % trips.size()];
    c.count("settled", astar(city, a, b).settled);
  }
});
