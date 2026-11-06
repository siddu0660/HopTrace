# Reference

- [CMake](#cmake)
- [Writing targets](#writing-targets)
- [Benchmark flags](#benchmark-flags)
- [Spec files](#spec-files)
- [Presets](#presets)
- [Output](#output)
- [`hoptrace.yaml` and `hoptrace run`](#hoptraceyaml-and-hoptrace-run)
- [`hoptrace` CLI](#hoptrace-cli)
- [Files written](#files-written)
- [How measurement works](#how-measurement-works)

## CMake

These are available after `add_subdirectory(hoptrace)`, `FetchContent_MakeAvailable(hoptrace)` or `find_package(HopTrace)`.

| Command | What it does |
|---|---|
| `hoptrace_add_benchmark(<name> TARGETS <files> [LINK <libs>] [INSTRUMENT <targets>])` | Builds executable `<name>` with HopTrace's runner as `main()`. `TARGETS` are files containing `HT_TARGET(...)`: `.cpp` files are compiled, and headers are `#include`d into a generated source. `LINK` is linked privately. Each `INSTRUMENT` target is compiled with hop hooks. Records the git SHA of the top-level project in the run metadata. |
| `hoptrace_instrument(<target>)` | Compiles `<target>` with hop hooks. GCC: `-finstrument-functions` with system headers excluded. Clang: `-finstrument-functions-after-inlining`. |
| `HopTrace::hoptrace` | The library: header `<hoptrace/hoptrace.hpp>`, static lib, links `-rdynamic` and `dl`. Link it yourself only if you don't use `hoptrace_add_benchmark`. |

| Option | Default | |
|---|---|---|
| `HOPTRACE_BUILD_TESTS` | ON when top-level | HopTrace's own tests and the order book example |
| `HOPTRACE_INSTALL` | ON when top-level | install rules: lib, headers, `lib/cmake/HopTrace`, `bin/hoptrace` |
| `HT_SANITIZE` | empty | e.g. `address,undefined` or `thread` |

## Writing targets

```cpp
HT_TARGET(name, [](ht::Ctx& c) { /* setup */ for (auto _ : c) { /* one timed call */ } /* teardown */ });
HT_TARGET(name, [] { /* one timed call */ });     // shorthand: no setup
```

`ht::Ctx` is handed to the target once per worker:

| Member | |
|---|---|
| `size` | `--size` |
| `i` | iteration number on this worker (warmup included) |
| `worker` | 0 .. workers−1 across the run |
| `rng` | `std::mt19937_64` seeded from `--seed` + worker |
| `uniform(a, b)`, `normal(mean, sd)`, `lognormal(mean, sigma)` | random doubles (`lognormal` takes the real mean) |
| `zipf(n, s)` | 0..n−1, rank k with weight 1/(k+1)^s |
| `key(n)` | 0..n−1 from `--key-dist` |
| `draw_size()` | per-call size from `--size-dist` around `--size` |
| `inputs(gen)` | `std::vector` of `size` items from `gen(rng, index)` |
| `dataset()` | lines of `--dataset` (loaded once, shared, read-only) |
| `fail()` | marks the current call as an error (counted, still timed) |
| `count(name, v = 1)` | adds to a custom metric, summed per target (warmup calls are not counted) |

C++ API, for driving runs from your own code or tests: `ht::profile(fn, spec)`, `ht::profile_mix({{fn, weight}, ...}, spec)`, `ht::tail_diff(root)`, `ht::complexity(points)`, `ht::percentile(v, p)`. See `include/hoptrace/hoptrace.hpp`.

## Benchmark flags

Times accept `ns`, `us`, `ms`, `s`, `m` and `h` (a bare number is seconds). Counts accept `1e5`.

**Volume and concurrency**

| Flag | Default | |
|---|---|---|
| `--calls=N` | 10000 | timed calls in total (split across workers) |
| `--warmup=N` | 1000 | untimed calls first |
| `--duration=T` | – | also stop after T; whichever limit is hit first |
| `--workers=N` | 1 | caller threads |
| `--pin=compact\|0,2,4` | – | pin workers to cores (cyclic) |
| `--runs=N` | 1 | repeat; show the median run, p50 and p99 spread, `UNSTABLE` if the spread (CV) is over 10% |

**Arrivals**

| Flag | |
|---|---|
| `--rate=R` | calls/s in total. Setting it switches to open loop: latency counts from the intended start, so queueing is included (no coordinated omission). |
| `--schedule=…` | `constant` · `poisson` (both use `--rate`) · `ramp:FROM:TO:OVER` · `step:R1,R2,…:HOLD` · `spike:BASE:PEAK:AT:FOR` · `burst:SIZE:EVERY` |
| `--think=T` | closed loop: untimed pause between calls |
| `--stress` | open-loop ramp: doubles the rate from `--rate` (default 1000/s) until `--slo` breaks, and reports the saturation point |

**Inputs**

| Flag | Default | |
|---|---|---|
| `--size=small\|medium\|large\|N` | 1000 | 100 / 1e4 / 1e6 / N |
| `--size-dist=…` | fixed | `fixed` · `uniform` · `normal:SD` · `lognormal:SIGMA` · `zipf:S` (for `draw_size()`) |
| `--key-dist=…` | uniform | `uniform` · `zipf:S` (for `key()`) |
| `--dataset=file` | – | lines available as `dataset()` |
| `--seed=N` | 1 | |

**What to run**

| Flag | |
|---|---|
| `--list` | print target names |
| `--filter=substr` | only targets whose name contains it |
| `--only=a,b` | only these targets (exact names) |
| `--mix=a:70,b:30` | run these targets concurrently; calls, workers and rate are split by weight |
| `--sweep=knob:v1,v2;knob:…` | cartesian matrix over `workers`, `size`, `rate` and `calls`. Prints one summary row per cell. Size sweeps get a complexity fit. |

**Environment and faults**

| Flag | |
|---|---|
| `--cold` | stream through 32 MB before every timed call (cold caches) |
| `--noise=N` | N background CPU-burning threads |
| `--inject=fn:T,…` | spend T extra inside every function whose name contains `fn` |

**Measurement**

| Flag | |
|---|---|
| `--sample=N` | record hops for 1 in N calls (end-to-end latency for all) |
| `--timeout=T` | count calls slower than T (calls aren't aborted) |
| `--raw` | don't subtract hook overhead |
| `--top=K` | rows in "hot functions" (default 5) |
| `HT_CLOCK=steady` (env) | use `steady_clock` instead of the TSC |

**Gates and output**

| Flag | |
|---|---|
| `--slo=k:v,…` | `pNN:T` (any percentile) · `max:T` · `timeout_rate:F` · `error_rate:F`. Exit 1 on a breach. Checked on each target's end-to-end latency. |
| `--json=f` | metadata line plus one line per node ([format](#files-written)) |
| `--folded=f` | `a;b;c self_ns` lines for flamegraph.pl or speedscope |
| `--trace=f` | Chrome Trace Event JSON, one track per worker; open in ui.perfetto.dev |
| `--diff a b [--threshold=0.05] [--min-delta=250ns]` | same as `hoptrace diff` |
| `--spec=file`, `--preset=name` | see below |
| `--help` | usage |

Exit codes: `0` ok · `1` SLO breach (or regression, with `--diff`) · `2` bad arguments or file.

## Spec files

`--spec=file.yaml` takes the flags as keys (without `--`). This is the same format as one suite in [`hoptrace.yaml`](#hoptraceyaml-and-hoptrace-run). The order of precedence is **preset < spec file < CLI**.

```yaml
# comments are fine
calls: 2_000_000          # digit separators
workers: 16
pin: compact
schedule: ramp:10000:200000:60s
size-dist: lognormal:1.0
key-dist: zipf:1.1
timeout: 5ms
preset: cold              # presets work here too
mix:                      # blocks: mix, sweep, slo, inject
  insert: 70
  cancel: 20
  best_bid: 10
slo:
  p99: 200us
  p99.9: 1ms
  error_rate: 0.001
sweep:
  workers: [1, 2, 4, 8]   # lists for sweep values
  size: [100, 10_000]
inject:
  resort: 20us
```

Not supported: flow style (`{a: 1}`), anchors and multi-document files.

## Presets

| Preset | Expands to |
|---|---|
| `micro` | `--workers=1 --rate=0 --warmup=1000` |
| `load` | `--workers=4 --rate=10000` |
| `stress` | `--stress --workers=4` (needs `--slo`) |
| `concurrency` | `--sweep=workers:1,2,4,8` |
| `scaling` | `--sweep=size:100,1000,10000,100000` |
| `cold` | `--cold` |
| `soak` | `--duration=60s --calls=2e9 --rate=1000 --workers=2` (adds p50 drift and RSS growth to the report) |

## Output

The order is: run metadata, then hook overhead, then per target:

1. **Call tree:** `calls`, `p50`, `p99` and `max` are inclusive latency in µs. `self` is the mean exclusive time. `<target>` is the end-to-end latency of one iteration. Children are sorted by total time.
2. **Hot functions:** share of the target's total time spent in each function's own code, merged across call paths.
3. **Hot path:** follows the child with the most total time from the root down.
4. **Tail:** the functions whose mean self time grows the most in the slowest 1% of calls compared with p40–p60 calls. This is based on a sample of whole calls (20k per worker).
5. **Extras, when relevant:** sampling note, `--runs` spread, timeouts, errors, custom metrics, soak drift and RSS growth, SLO results.

A sweep prints one row per cell and target, plus `scaling … fits O(…)`. A stress run prints p50/p99 per rate step and the saturation line.

## `hoptrace.yaml` and `hoptrace run`

One file describes how a codebase is benchmarked, so nobody has to remember flags:

```yaml
bench: build/my_bench          # required; paths are relative to this file
build: cmake --build build -j  # optional shell command, run before the suites
out: .hoptrace                 # where results go (default .hoptrace)
threshold: 0.05                # "slower than last run" needs > +5% p50 ...
min_delta: 250ns               # ... and more than this absolute change
fail_on_regression: false      # true: a slower-than-last-run suite fails the run

defaults:                      # merged into every suite (a suite's own keys win)
  warmup: 1000

suites:
  <name>:                      # any keys from "Spec files": flags, blocks, spec:, preset:, only:, …
    only: insert
    calls: 50_000
    slo:
      p99: 200us
```

| Command | |
|---|---|
| `hoptrace run [suite…]` | build (if `build:` is set), then run the named suites, or all of them, in file order |
| `hoptrace ls [suite…]` | print each suite's resolved benchmark command |
| `-f path` | another file (default `./hoptrace.yaml`) |
| `--no-build` / `--no-diff` / `--strict` | skip the build step / skip the comparison / fail on "slower than last run" |
| `--bench=path`, `--out=dir` | override those keys |
| `-- flags…` | append flags to every suite for this run, e.g. `hoptrace run -- --calls=500` |

What happens on each run:
- **Results:** each suite writes `<out>/<suite>.jsonl`. The previous result is kept as `<suite>.prev.jsonl` and compared with the new one, the same as `hoptrace diff`.
- **Summary:** a table lists each suite as ok, SLO breached, slower than last run, regressed or failed, with its time.
- **Exit code:** 1 if any SLO broke (or a regression, with `fail_on_regression`), 2 if a suite failed to run.

Why "slower than last run" doesn't fail by default: separate processes of the same code can differ by 10–30% on a busy or shared machine, because memory layout and CPU state change from process to process. A single run's samples can't show that variance. To gate CI on regressions, turn on `fail_on_regression` on a quiet, pinned machine, with `runs: 3` or more and a baseline from the same machine.

## `hoptrace` CLI

```bash
hoptrace report run.jsonl [--top=K] [--filter=substr] [--folded=out.fold]   # re-print a saved run
hoptrace diff base.jsonl new.jsonl [--threshold=0.05] [--min-delta=250ns]   # exit 1 on regression
```

`diff` compares every hop present in both files with at least 30 samples. It flags a regression when p50 rose by more than the threshold and by at least `--min-delta` (default 250 ns, so sub-µs jitter doesn't count), and a Mann–Whitney U test gives p < 0.01.

## Files written

`--json`, one JSON object per line:

```json
{"meta":{"cpu":"…","cores":"8","governor":"performance","clock":"tsc 3.00 GHz","compiler":"13.3.0","build":"RelWithDebInfo -O2 -g","git":"a1b2c3d","host":"…","time":"…","seed":"1"}}
{"target":"insert","path":"insert","n":10000,"incl_sum":…,"self_sum":…,"max":…,"incl":[…]}
{"target":"insert","path":"insert;insert_order(OrderBook&, double)","n":10000,…}
{"tail":"insert","hops":[["resort(OrderBook&)",210300,24800],…]}
```

- **Paths:** `path` is the target followed by the call chain, separated by `;`.
- **Exact fields:** `n`, `incl_sum`, `self_sum` and `max` are exact.
- **Samples:** `incl` holds latency samples in ns, at most 1M per node, taken as a uniform reservoir.
- **Root rows** also carry `errors` and `timeouts`. `tail` lines hold the tail analysis: [function, slow-call self ns, typical self ns].

## How measurement works

- **Hooks:** instrumented functions call `__cyg_profile_func_enter` and `__cyg_profile_func_exit`. Each hook appends `{function, timestamp}` to a thread-local buffer. Nothing is locked or allocated on the hot path, except when the buffer grows.
- **Clock:** `rdtscp`, calibrated against `steady_clock` at startup, when the CPU reports an invariant TSC. Otherwise `steady_clock`.
- **Overhead subtraction:** at startup HopTrace times an empty instrumented function and a function that calls it. That gives the cost each hop adds to its own span and to its parent's, which is subtracted from every node. The end-to-end `<target>` latency of each recorded call is corrected the same way, by the number of hops it fired, so code with many tiny instrumented functions isn't reported as slower than it is.
- **Bounded memory:** events are folded into the calling-context tree every ~1M events, between calls. Each node keeps exact count, sums and max, plus a 1M reservoir sample for percentiles.
- **Exceptions:** an exception skips exit hooks. The replay pops frames until it finds the matching one.
- **Open loop:** call k starts at its scheduled time. If the system falls behind, the delay counts toward latency, which is how coordinated omission is avoided. In open loop, time spent folding events or flushing caches between calls also counts as queueing.
