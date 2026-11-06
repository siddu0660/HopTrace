# Getting started

This walks through profiling a small codebase from scratch, in about 10 minutes. The finished project is in [`examples/consumer`](../examples/consumer), and you can build it as-is.

## 1. The code under test

Any C++ library works. Here is a tiny one, `textstats`:

```cpp
// textstats.hpp
std::vector<std::string> tokenize(const std::string& text);
std::map<std::string, int> word_counts(const std::string& text);   // calls tokenize()
std::string most_common(const std::string& text);                  // calls word_counts()
```

You don't change any of its code.

## 2. Get HopTrace

Either install it once:

```bash
git clone <hoptrace repo> && cd hoptrace
cmake -S . -B build -DHOPTRACE_BUILD_TESTS=OFF && cmake --build build -j
cmake --install build --prefix ~/.local          # gives find_package(HopTrace) and the `hoptrace` CLI
```

or vendor it in your tree (git submodule or FetchContent) and use `add_subdirectory`.

## 3. Describe the benchmark in CMake

```cmake
cmake_minimum_required(VERSION 3.20)
project(textstats CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE RelWithDebInfo)   # optimised, but keeps symbols for readable names

find_package(HopTrace REQUIRED)        # or: add_subdirectory(third_party/hoptrace)

add_library(textstats textstats.cpp)

hoptrace_add_benchmark(textstats_bench
    TARGETS    targets.hpp      # entry points (step 4)
    LINK       textstats        # what they call
    INSTRUMENT textstats)       # compiled with hop hooks: these functions show up in the call tree
```

Only the targets listed under `INSTRUMENT` get hooks. Your other code and the standard library run at full speed and don't appear as hops.

## 4. List the entry points

`targets.hpp` holds one `HT_TARGET` per function you want to start from:

```cpp
#include <hoptrace/hoptrace.hpp>
#include "textstats.hpp"

static const char* kWords[] = {"latency", "hop", "trace", "call", "tree", "profile", "p99", "tail"};

static std::string make_text(ht::Ctx& c, size_t words) {
  std::string t;
  for (size_t i = 0; i < words; i++) { t += kWords[c.key(8)]; t += ' '; }   // key(): popularity from --key-dist
  return t;
}

HT_TARGET(most_common, [](ht::Ctx& c) {
  std::string text = make_text(c, c.size);   // untimed setup: runs once per worker, --size words
  for (auto _ : c) most_common(text);        // each iteration = one timed call
});
```

How a target works:
- **Setup and teardown:** everything before `for (auto _ : c)` is setup, and everything after it is teardown. Neither is timed or shown in the tree.
- **Inputs:** `c.size`, `c.rng`, `c.key()`, `c.draw_size()`, `c.inputs()` and `c.dataset()` give you reproducible inputs. The random seed comes from `--seed`, offset by worker.
- **Failures and metrics:** call `c.fail()` when a call fails, so it is counted for the `error_rate` SLO. `c.count("bytes", n)` adds a custom metric.
- **Multi-step scenarios:** write them as one target that makes the calls in order. Each step shows up as its own hop.

## 5. Build and run

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=~/.local && cmake --build build -j
./build/textstats_bench --list
./build/textstats_bench --calls=3000 --size=500 --key-dist=zipf:1.2 --filter=most
```

You get the call tree (see the screenshot in the README):

- **calls, p50, p99, max:** inclusive latency of each hop, in µs. The `<target>` row is the end-to-end latency of one iteration.
- **self:** mean time spent in that function itself, excluding its children.
- **hot functions:** share of total time spent in each function's own code.
- **hot path:** the heaviest chain from the root to a leaf.
- **tail:** which functions take the most extra time in the slowest 1% of calls compared with typical (p40–p60) calls. This is where to look for p99 problems.

The header block records the CPU, governor, clock, compiler and flags, git SHA, host, time and seed, so you can compare results later.

## 6. Put it under load

```bash
./build/textstats_bench --workers=4 --rate=20000 --schedule=poisson   # open loop: latency counts queueing
./build/textstats_bench --preset=stress --slo=p99:200us                # find the rate where the SLO breaks
./build/textstats_bench --preset=scaling                               # size 100..1e5, fits O(1)..O(n^2)
./build/textstats_bench --runs=5 --pin=compact                         # repeat; flags unstable results
```

Keep settings in a spec file, using the same keys as the flags:

```yaml
# load.yaml
calls: 200_000
workers: 8
rate: 50_000
schedule: ramp:10000:50000:30s
key-dist: zipf:1.1
slo:
  p99: 200us
  error_rate: 0.001
```

```bash
./build/textstats_bench --spec=load.yaml --calls=5e4    # CLI flags override the file
```

## 7. Put it all in `hoptrace.yaml`

Instead of remembering flags, describe the suites once, next to your `CMakeLists.txt`:

```yaml
bench: build/textstats_bench
build: cmake --build build -j

defaults:
  size: 500
  key-dist: zipf:1.2

suites:
  smoke:
    sweep:
      calls: [10, 100, 1_000]
    slo:
      error_rate: 0
  load:
    only: most_common
    workers: 4
    rate: 20_000
    schedule: poisson
    slo:
      p99: 200us
```

```bash
hoptrace run          # build, run every suite, compare with the last run, summary table
hoptrace run smoke    # one suite
hoptrace ls           # see the exact command each suite runs
```

[`examples/route_planner/hoptrace.yaml`](../examples/route_planner/hoptrace.yaml) is a complete example.

## 8. Save, compare, gate CI

```bash
./build/textstats_bench --json=base.jsonl                 # on main
./build/textstats_bench --json=new.jsonl                  # on your branch
hoptrace diff base.jsonl new.jsonl --threshold=0.05       # exit 1 if any hop got >5% slower (p < 0.01)
hoptrace report new.jsonl --folded=new.fold               # re-print a run; folded stacks for flamegraph.pl/speedscope
./build/textstats_bench --trace=run.json                  # timeline: open in ui.perfetto.dev
```

## Tips for stable numbers

- **Build type:** use `RelWithDebInfo` (optimised, with symbols). Debug builds measure the wrong thing.
- **Hardware:** use the `performance` CPU governor, `--pin=compact`, and, if you can, cores isolated with `isolcpus`. HopTrace prints a tip when the governor isn't `performance`.
- **Repeat runs:** use `--runs=5`. A run flagged `UNSTABLE` (p50 varies more than 10% across runs) shouldn't be compared.
- **Function names:** hops appear by name only for functions with external linkage. `static` functions and functions in anonymous namespaces show as hex addresses.
