/// @file hoptrace.hpp
/// @brief HopTrace: per-hop latency profiling and load testing for C++ code. The only header targets need.
///
/// @mainpage HopTrace
/// HopTrace calls your C++ entry points under the load you choose and measures the latency of every function they
/// call on the way down ("hops"), then reports the call tree with percentiles, the hot path and tail analysis.
///
/// - Declare entry points with HT_TARGET (target.hpp); each gets an ht::Ctx.
/// - Shape a run with ht::Spec (spec.hpp); run it from code with ht::profile() (profile.hpp) or from the command
///   line through ht::main() (cli.hpp).
/// - Results are ht::Node call trees (node.hpp); saved runs load back with ht::load_run() (saved_run.hpp).
///
/// See docs/getting-started.md and docs/reference.md for the user guide.
#pragma once

#include "cli.hpp"
#include "node.hpp"
#include "profile.hpp"
#include "saved_run.hpp"
#include "spec.hpp"
#include "target.hpp"
