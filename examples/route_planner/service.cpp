#include "service.hpp"

#include <cmath>
#include <cstdio>

Request parse_request(const std::string& q) {
  Request r;
  size_t i = 0;
  while (i < q.size()) {
    size_t amp = q.find('&', i), eq = q.find('=', i);
    if (amp == std::string::npos) amp = q.size();
    if (eq != std::string::npos && eq < amp) {
      std::string k = q.substr(i, eq - i), v = q.substr(eq + 1, amp - eq - 1);
      if (k == "from")
        r.from = std::stoi(v);
      else if (k == "to")
        r.to = std::stoi(v);
      else if (k == "mode")
        r.fast = v != "exact";
    }
    i = amp + 1;
  }
  return r;
}

double price_ride(const Route& r, double surge) {
  double fare = 2.50 + 0.35 * r.minutes + 0.02 * (double)r.path.size();
  return std::round(fare * surge * 100) / 100;
}

std::string format_response(const Route& r, double fare) {
  char buf[128];
  snprintf(buf, sizeof buf, "{\"minutes\":%.1f,\"stops\":%zu,\"fare\":%.2f}", r.minutes, r.path.size(), fare);
  return buf;
}

const Route* RouteCache::find(int from, int to) {
  auto it = index_.find((Key)from << 32 | (uint32_t)to);
  if (it == index_.end()) return nullptr;
  order_.splice(order_.begin(), order_, it->second);  // most recently used to the front
  return &it->second->second;
}

void RouteCache::put(int from, int to, Route r) {
  Key k = (Key)from << 32 | (uint32_t)to;
  if (index_.count(k)) return;
  order_.emplace_front(k, std::move(r));
  index_[k] = order_.begin();
  if (order_.size() > cap_) {
    index_.erase(order_.back().first);
    order_.pop_back();
  }
}

RouteService::RouteService(int intersections, uint64_t seed, size_t cache_size)
    : city_(make_city(intersections, seed)), cache_(cache_size) {}

std::string RouteService::handle(const std::string& query) {
  Request req = parse_request(query);
  if (req.from < 0 || req.to < 0 || req.from >= (int)city_.adj.size() || req.to >= (int)city_.adj.size()) return "";
  if (++served_ % 1000 == 0) surge_ = 1.0 + 0.5 * std::sin((double)served_ / 5000);  // demand cycle
  const Route* r = cache_.find(req.from, req.to);
  Route fresh;
  if (r)
    hits++;
  else {
    misses++;
    fresh = req.fast ? astar(city_, req.from, req.to) : dijkstra(city_, req.from, req.to);
    cache_.put(req.from, req.to, fresh);
    r = &fresh;
  }
  if (r->path.empty()) return "";
  return format_response(*r, price_ride(*r, surge_));
}
