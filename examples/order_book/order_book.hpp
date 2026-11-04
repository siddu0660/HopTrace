#pragma once
#include <vector>
struct OrderBook {
  std::vector<double> bids;
};
void insert_order(OrderBook&, double price);
double best_bid(OrderBook&);
