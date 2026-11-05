# route_planner: a sample codebase to try HopTrace on

This is a small ride-routing service. Each request goes through these steps:
- **parse:** read the query string;
- **cache:** look the route up in an LRU cache;
- **route on a miss:** run A* (or Dijkstra for `mode=exact`) over a generated city road graph;
- **price:** compute the fare with surge pricing;
- **format:** build the response.

| File | What it is |
|---|---|
| `city.*` | a road graph: jittered grid streets with traffic, plus a few highways |
| `routing.*` | Dijkstra and A* (shared best-first search; the A* heuristic is admissible) |
| `service.*` | request parsing, LRU route cache, pricing, response formatting |
| `targets.hpp` | the HopTrace entry points: `handle_request`, `dijkstra`, `astar` |
| `load.yaml` | an open-loop load spec |

None of `city/routing/service` knows about HopTrace. `CMakeLists.txt` has the whole integration in one call:
`hoptrace_add_benchmark(route_bench TARGETS targets.hpp LINK route_planner INSTRUMENT route_planner)`.

## Build

From a HopTrace checkout:
```bash
cmake -S examples/route_planner -B rp -DHOPTRACE_SOURCE=$PWD && cmake --build rp -j
```
or against an installed HopTrace:
```bash
cmake -S examples/route_planner -B rp -DCMAKE_PREFIX_PATH=<prefix> && cmake --build rp -j
```

## The easy way: `hoptrace.yaml`

`hoptrace.yaml` here holds four suites (smoke, cache, algorithms, load) plus the build command:
```bash
cd examples/route_planner
../../build/hoptrace ls          # what each suite runs
../../build/hoptrace run         # build, run all suites, compare with the last run
../../build/hoptrace run smoke   # one suite
```
(`hoptrace` is the CLI from the HopTrace build; after `cmake --install` it's on your PATH.)

## Things to try by hand

```bash
./rp/route_bench --list
./rp/route_bench --filter=handle --size=2500 --key-dist=zipf:1.1 --calls=3000   # popular trips -> cache hits
./rp/route_bench --filter=a --size=2500 --calls=1000                            # matches astar, dijkstra (and handle_request)
./rp/route_bench --preset=scaling --sweep=size:400,1600,6400,25600 --filter=a   # how each algorithm scales with city size
./rp/route_bench --spec=examples/route_planner/load.yaml                        # 4 workers, Poisson 8000/s, SLO gate
./rp/route_bench --filter=handle --size=2500 --key-dist=zipf:1.1 --trace=rp.json   # timeline for ui.perfetto.dev
```

## What you should see

- **Cache hits vs misses:** with `--key-dist=zipf:1.1` about 75% of requests are cache hits (`metric cache_hit`). The p50 is around 1 µs, while the p99 is hundreds of µs.
- **The call tree explains the gap:** `astar` and `dijkstra` only run on misses (their `calls` column is the miss count). Inside them, `search` dominates, followed by the heuristic `straight_line_minutes` → `distance_km`, called about 800 times per A* search.
- **Tail analysis names the culprit:** the slowest 1% of requests spend their extra time in `search` and the heuristic, so the remedy is fewer misses (a bigger cache) or cheaper searches.
- **A\* vs Dijkstra:** A* settles about half as many nodes (`metric settled`, about 590 vs 1255 per call at 2500 intersections) and has the lower p50. Under `--preset=scaling`, Dijkstra fits O(n log n) and A* grows more slowly.
- **Under load (`load.yaml`):** open-loop arrivals queue behind slow misses, so the p99 and the timeout count show it. Try a bigger cache in `targets.hpp` or fewer workers and compare with `hoptrace diff`.

Some lines are tiny compiler-generated functions (`Route::Route(Route&&)`, `~Route()`). They come from inline code in instrumented files, and their cost is real but small.
