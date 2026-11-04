#include <algorithm>

#include <hoptrace/hoptrace.hpp>
#include "order_book.hpp"

// Setup before `for (auto _ : c)` is untimed and not recorded; each iteration is one timed call.
static double price(std::mt19937_64& rng, size_t) {
  return 90.0 + (double)(rng() % 2000) / 100.0;
}

HT_TARGET(order_book_insert, [](ht::Ctx& c) {
  auto prices = c.inputs(price);  // c.size prices, seeded per worker
  OrderBook ob;
  ob.bids = prices;  // book starts --size deep (untimed)
  std::sort(ob.bids.begin(), ob.bids.end());
  for (auto _ : c) {
    insert_order(ob, prices[c.i % prices.size()]);
    ob.bids.pop_back();  // stay at --size so latency doesn't drift
  }
});

HT_TARGET(order_book_best_bid, [](ht::Ctx& c) {
  OrderBook ob;
  ob.bids = c.inputs(price);
  std::sort(ob.bids.begin(),
            ob.bids.end());  // build the book once, not via insert_order: keeps --size=large setup fast
  for (auto _ : c) best_bid(ob);
});
