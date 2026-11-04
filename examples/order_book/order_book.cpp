#include "order_book.hpp"
#include <algorithm>

__attribute__((noinline)) bool validate(double p) {
  return p > 0 && p < 1e9;
}
__attribute__((noinline)) void resort(OrderBook& ob) {
  std::sort(ob.bids.begin(), ob.bids.end());
}
void insert_order(OrderBook& ob, double price) {
  if (!validate(price)) return;
  ob.bids.push_back(price);
  resort(ob);
}
double best_bid(OrderBook& ob) {
  return ob.bids.empty() ? 0 : ob.bids.back();
}
