/// @file cli.cpp
/// @brief ht::cli(): the hoptrace tool (run/ls suites from hoptrace.yaml, report and diff saved runs).
#include "detail.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace ht {

using namespace detail;

namespace {

/// One named suite from hoptrace.yaml, resolved to benchmark flags (defaults first, then its own keys).
struct Suite {
  std::string name;
  std::vector<std::string> flags;
};
/// A loaded hoptrace.yaml.
struct Compose {
  std::string file, bench, build, out = ".hoptrace";
  double threshold = 0.05, min_delta = 250;
  bool fail_on_regression = false;  // separate processes vary more than one run's samples suggest, so off by default
  std::vector<Suite> suites;
};

/// Loads hoptrace.yaml. @throws std::runtime_error on unknown keys, a missing bench, or no suites
Compose load_compose(const std::string& file) {
  Compose c;
  c.file = file;
  YNode root = read_yaml(file);
  const YNode* defaults = nullptr;
  for (auto& k : root.kids) {
    if (k.key == "bench")
      c.bench = k.value;
    else if (k.key == "build")
      c.build = k.value;
    else if (k.key == "out")
      c.out = k.value;
    else if (k.key == "threshold")
      c.threshold = atof(k.value.c_str());
    else if (k.key == "min_delta")
      c.min_delta = parse_time(k.value.c_str()) * 1e9;
    else if (k.key == "fail_on_regression")
      c.fail_on_regression = k.value == "true" || k.value == "yes" || k.value == "1";
    else if (k.key == "defaults")
      defaults = &k;
    else if (k.key != "suites")
      throw std::runtime_error(file + ":" + std::to_string(k.line) + ": unknown key '" + k.key +
                               "' (bench, build, out, threshold, min_delta, fail_on_regression, defaults, suites)");
  }
  for (auto& k : root.kids) {
    if (k.key != "suites") continue;
    for (auto& s : k.kids) {
      Suite su{s.key, {}};
      if (defaults) yaml_flags(*defaults, file, su.flags);
      yaml_flags(s, file, su.flags);  // later flags win in the benchmark's parser
      c.suites.push_back(std::move(su));
    }
  }
  if (c.bench.empty()) throw std::runtime_error(file + ": 'bench: <path to benchmark executable>' is required");
  if (c.suites.empty()) throw std::runtime_error(file + ": no suites (add a 'suites:' block)");
  return c;
}

/// Runs a command with inherited stdout/stderr. @return its exit code
int run_command(const std::vector<std::string>& args) {  // inherits stdout/stderr; returns exit code
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    perror(argv[0]);
    _exit(127);
  }
  if (pid < 0) return 127;
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

bool exists(const std::string& p) {
  struct stat sb;
  return stat(p.c_str(), &sb) == 0;
}

/// `hoptrace run` / `hoptrace ls`: builds, runs the suites, compares each with its previous run, prints a summary.
int compose_main(const std::string& cmd, int argc, char** argv) {
  std::string file = "hoptrace.yaml", bench_override, out_override;
  bool build = true, do_diff = true, strict = false;
  std::vector<std::string> want, extra;  // extra: flags after `--`, appended to every suite
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--") {
      extra.assign(argv + i + 1, argv + argc);
      break;
    }
    if (a == "-f" && i + 1 < argc)
      file = argv[++i];
    else if (a.rfind("--file=", 0) == 0)
      file = a.substr(7);
    else if (a.rfind("--bench=", 0) == 0)
      bench_override = a.substr(8);
    else if (a.rfind("--out=", 0) == 0)
      out_override = a.substr(6);
    else if (a == "--no-build")
      build = false;
    else if (a == "--no-diff")
      do_diff = false;
    else if (a == "--strict")
      strict = true;
    else if (a[0] == '-') {
      fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    } else
      want.push_back(a);
  }
  Compose c;
  try {
    c = load_compose(file);
  } catch (const std::exception& e) {
    fprintf(stderr, "%s\n", e.what());
    return 2;
  }
  // paths in the file are relative to it; overrides given on the command line are relative to where we were run
  auto absolute = [](const std::string& p) {
    if (p.empty() || p[0] == '/') return p;
    char cwd[4096];
    return std::string(getcwd(cwd, sizeof cwd) ? cwd : ".") + "/" + p;
  };
  bench_override = absolute(bench_override);
  out_override = absolute(out_override);
  size_t slash = file.rfind('/');
  if (slash != std::string::npos && chdir(file.substr(0, slash).c_str()) != 0) {
    perror(file.c_str());
    return 2;
  }
  if (strict) c.fail_on_regression = true;
  if (!bench_override.empty()) c.bench = bench_override;
  if (!out_override.empty()) c.out = out_override;
  if (c.bench.find('/') == std::string::npos) c.bench = "./" + c.bench;

  std::vector<const Suite*> todo;
  for (auto& w : want) {
    auto it = std::find_if(c.suites.begin(), c.suites.end(), [&](auto& s) { return s.name == w; });
    if (it == c.suites.end()) {
      fprintf(stderr, "no suite '%s' in %s (have:", w.c_str(), file.c_str());
      for (auto& s : c.suites) fprintf(stderr, " %s", s.name.c_str());
      fprintf(stderr, ")\n");
      return 2;
    }
    todo.push_back(&*it);
  }
  if (want.empty())
    for (auto& s : c.suites) todo.push_back(&s);

  if (cmd == "ls") {
    for (auto* s : todo) {
      printf("%-20s %s", s->name.c_str(), c.bench.c_str());
      for (auto& f : s->flags) printf(" %s", f.c_str());
      printf("\n");
    }
    return 0;
  }
  if (build && !c.build.empty()) {
    printf("\033[1m▶ build:\033[0m %s\n", c.build.c_str());
    fflush(stdout);
    if (int rc = system(c.build.c_str()); rc != 0) {
      fprintf(stderr, "build failed (%d)\n", rc);
      return 2;
    }
  }
  if (!exists(c.bench)) {
    fprintf(stderr, "benchmark %s not found (build it, or set 'build:' in %s)\n", c.bench.c_str(), file.c_str());
    return 2;
  }
  mkdir(c.out.c_str(), 0755);

  struct Result {
    std::string name, outcome;
    double secs;
    int code;
    bool slower;
  };
  std::vector<Result> results;
  int worst = 0;
  for (auto* s : todo) {
    std::string json = c.out + "/" + s->name + ".jsonl", prev = c.out + "/" + s->name + ".prev.jsonl";
    if (exists(json)) rename(json.c_str(), prev.c_str());
    std::vector<std::string> args{c.bench};
    args.insert(args.end(), s->flags.begin(), s->flags.end());
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back("--json=" + json);
    printf("\n\033[1;36m▶ suite %s:\033[0m", s->name.c_str());
    for (auto& a : args) printf(" %s", a.c_str());
    printf("\n");
    auto t0 = std::chrono::steady_clock::now();
    int rc = run_command(args);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string outcome = rc == 0 ? "ok" : rc == 1 ? "SLO breached" : "failed (exit " + std::to_string(rc) + ")";
    bool slower = false;
    if (rc <= 1 && do_diff && exists(prev) && exists(json)) {
      printf("\033[1m--- %s vs previous run\033[0m\n", s->name.c_str());
      try {
        if (diff(prev.c_str(), json.c_str(), c.threshold, c.min_delta)) {
          slower = true;
          outcome = (rc == 0 ? std::string("") : outcome + ", ") +
                    (c.fail_on_regression ? "regressed" : "slower than last run");
          if (c.fail_on_regression) rc = 1;
        }
      } catch (const std::exception& e) {
        printf("(no diff: %s)\n", e.what());
      }
    }
    worst = std::max(worst, rc);
    results.push_back({s->name, outcome, secs, rc, slower});
  }
  printf("\n\033[1m%-20s %-30s %8s\033[0m\n", "suite", "result", "time");
  for (auto& r : results)
    printf("%-20s %s%-30s\033[0m %7.1fs\n", r.name.c_str(),
           r.code     ? "\033[31m"
           : r.slower ? "\033[33m"
                      : "\033[32m",
           r.outcome.c_str(), r.secs);
  printf("results in %s/<suite>.jsonl (previous run: <suite>.prev.jsonl)\n", c.out.c_str());
  if (std::any_of(results.begin(), results.end(), [](auto& r) { return r.slower && !r.code; }))
    printf(
        "note: run-to-run differences between processes can be 10-30%% on a busy machine; set fail_on_regression: "
        "true\n"
        "      (or --strict) to fail on them, ideally on a quiet, pinned machine with runs: 3+\n");
  return worst > 1 ? 2 : worst;
}

}  // namespace

int cli(int argc, char** argv) {
  auto usage = [&] {
    fprintf(stderr,
            "usage: %s run [suite...] [-f hoptrace.yaml] [--no-build] [--no-diff] [--strict] [-- extra flags]   (run "
            "suites from hoptrace.yaml)\n"
            "       %s ls [-f hoptrace.yaml]                                       (show suites and their commands)\n"
            "       %s report <run.jsonl> [--top=K] [--filter=substr] [--folded=out.fold]\n"
            "       %s diff <base.jsonl> <new.jsonl> [--threshold=0.05] [--min-delta=250ns]\n"
            "Runs are written by a benchmark with --json=<file>.\n",
            argv[0], argv[0], argv[0], argv[0]);
    return 2;
  };
  if (argc >= 2 && (!strcmp(argv[1], "run") || !strcmp(argv[1], "ls"))) return compose_main(argv[1], argc, argv);
  if (argc < 3) return usage();
  std::string cmd = argv[1];
  int top = 5;
  double threshold = 0.05, min_delta = 250;
  std::string filter, fold;
  std::vector<const char*> files;
  for (int i = 2; i < argc; i++) {
    const char* a = argv[i];
    if (!strncmp(a, "--top=", 6))
      top = atoi(a + 6);
    else if (!strncmp(a, "--filter=", 9))
      filter = a + 9;
    else if (!strncmp(a, "--folded=", 9))
      fold = a + 9;
    else if (!strncmp(a, "--threshold=", 12))
      threshold = atof(a + 12);
    else if (!strncmp(a, "--min-delta=", 12))
      min_delta = parse_time(a + 12) * 1e9;
    else if (a[0] == '-') {
      fprintf(stderr, "unknown option %s\n", a);
      return usage();
    } else
      files.push_back(a);
  }
  try {
    if (cmd == "diff" && files.size() == 2) return diff(files[0], files[1], threshold, min_delta);
    if (cmd != "report" || files.size() != 1) return usage();
    SavedRun run = load_run(files[0]);
    if (!run.meta.empty()) {
      printf("run:");
      for (auto& [k, v] : run.meta)
        if (!v.empty()) printf(" %s=%s", k.c_str(), v.c_str());
      printf("\n");
    }
    FILE* ff = fold.empty() ? nullptr : fopen(fold.c_str(), "w");
    if (!fold.empty() && !ff) {
      perror(fold.c_str());
      return 2;
    }
    int shown = 0;
    for (auto& [target, root] : run.targets) {
      if (target.find(filter) == std::string::npos) continue;
      printf("\n=== %s (calls=%zu) ===\n", target.c_str(), root.calls());
      print(root, top);
      auto t = run.tail.find(target);
      if (t != run.tail.end() && !t->second.empty()) {
        printf("tail: slowest 1%% of calls vs typical (p40-p60), extra self time per call:\n");
        for (auto& h : t->second)
          printf("  %+10.2fus  %s  (%.2fus vs %.2fus)\n", (h.slow_ns - h.typical_ns) / 1e3, h.name.c_str(),
                 h.slow_ns / 1e3, h.typical_ns / 1e3);
      }
      if (ff) folded(root, target, ff);
      shown++;
    }
    if (ff) fclose(ff);
    if (!shown) {
      fprintf(stderr, "no targets in %s%s\n", files[0], filter.empty() ? "" : " match --filter");
      return 1;
    }
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "%s\n", e.what());
    return 2;
  }
}

}  // namespace ht
