#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

#include "routing.hpp"

// A ride-routing service: parse request -> cache lookup -> route on miss -> price -> response.
struct Request {
  int from = -1, to = -1;
  bool fast = true;
};

Request parse_request(const std::string& query);  // "from=12&to=873&mode=fast|exact"
double price_ride(const Route& r, double surge);  // base + per-minute + per-stop, times surge
std::string format_response(const Route& r, double fare);

class RouteCache {  // LRU of (from, to) -> route
 public:
  explicit RouteCache(size_t capacity) : cap_(capacity) {}
  const Route* find(int from, int to);
  void put(int from, int to, Route r);

 private:
  using Key = uint64_t;
  size_t cap_;
  std::list<std::pair<Key, Route>> order_;
  std::unordered_map<Key, std::list<std::pair<Key, Route>>::iterator> index_;
};

class RouteService {
 public:
  RouteService(int intersections, uint64_t seed, size_t cache_size);
  std::string handle(const std::string& query);  // empty string = bad request or no route
  const City& city() const { return city_; }
  uint64_t hits = 0, misses = 0;

 private:
  City city_;
  RouteCache cache_;
  double surge_ = 1.0;
  uint64_t served_ = 0;
};
