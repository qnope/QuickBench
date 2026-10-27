// QuickBench: a tiny, header-only subset of the Google Benchmark API.
//
// Compiler Explorer does not ship a linkable libbenchmark for every compiler,
// so this header is prepended to the user code instead. It supports the parts
// of the API that micro-benchmarks usually need and prints results in the
// Google Benchmark JSON format (--benchmark_format=json) or as a console table.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace benchmark {

enum TimeUnit { kNanosecond, kMicrosecond, kMillisecond, kSecond };

using IterationCount = std::int64_t;

template <class Tp>
inline __attribute__((always_inline)) void DoNotOptimize(Tp const& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

template <class Tp>
inline __attribute__((always_inline)) void DoNotOptimize(Tp& value) {
#if defined(__clang__)
  asm volatile("" : "+r,m"(value) : : "memory");
#else
  asm volatile("" : "+m,r"(value) : : "memory");
#endif
}

inline __attribute__((always_inline)) void ClobberMemory() {
  asm volatile("" : : : "memory");
}

namespace internal {

inline double CpuNow() {
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

inline double RealNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace internal

class State {
 public:
  struct Value {
    Value() {}
    ~Value() {}
  };

  struct Iterator {
    IterationCount remaining;
    State* parent;

    Value operator*() const { return {}; }
    Iterator& operator++() {
      --remaining;
      return *this;
    }
    bool operator!=(const Iterator&) const {
      if (__builtin_expect(remaining != 0, 1)) return true;
      parent->FinishKeepRunning();
      return false;
    }
  };

  State(IterationCount max_iters, std::vector<std::int64_t> ranges)
      : max_iterations_(max_iters), ranges_(std::move(ranges)) {}

  Iterator begin() {
    StartKeepRunning();
    return {max_iterations_, this};
  }
  Iterator end() { return {0, this}; }

  bool KeepRunning() { return KeepRunningBatch(1); }
  bool KeepRunningBatch(IterationCount n) {
    if (!started_) StartKeepRunning();
    if (done_ + n <= max_iterations_ && !error_) {
      done_ += n;
      return true;
    }
    FinishKeepRunning();
    return false;
  }

  void PauseTiming() {
    if (!running_) return;
    real_ += internal::RealNow() - real_start_;
    cpu_ += internal::CpuNow() - cpu_start_;
    running_ = false;
  }
  void ResumeTiming() {
    if (running_) return;
    real_start_ = internal::RealNow();
    cpu_start_ = internal::CpuNow();
    running_ = true;
  }

  void SetIterationTime(double seconds) { manual_ += seconds; }
  void SkipWithError(const char* msg) {
    error_ = true;
    error_message_ = msg;
  }
  void SkipWithMessage(const char* msg) { SkipWithError(msg); }
  bool error_occurred() const { return error_; }

  std::int64_t range(std::size_t i = 0) const { return i < ranges_.size() ? ranges_[i] : 0; }
  std::int64_t range_x() const { return range(0); }
  std::int64_t range_y() const { return range(1); }
  IterationCount iterations() const { return finished_ ? max_iterations_ : done_; }
  IterationCount max_iterations() const { return max_iterations_; }

  void SetItemsProcessed(std::int64_t n) { items_ = n; }
  void SetBytesProcessed(std::int64_t n) { bytes_ = n; }
  void SetComplexityN(std::int64_t) {}
  void SetLabel(const std::string& label) { label_ = label; }
  std::int64_t items_processed() const { return items_; }
  std::int64_t bytes_processed() const { return bytes_; }

  std::map<std::string, double> counters;

  // Results, read by the runner.
  double real_time() const { return real_; }
  double cpu_time() const { return cpu_; }
  double manual_time() const { return manual_; }
  const std::string& label() const { return label_; }
  const std::string& error_message() const { return error_message_; }

 private:
  void StartKeepRunning() {
    started_ = true;
    ResumeTiming();
  }
  void FinishKeepRunning() {
    if (finished_) return;
    PauseTiming();
    finished_ = true;
  }

  IterationCount max_iterations_;
  IterationCount done_ = 0;
  std::vector<std::int64_t> ranges_;
  bool started_ = false, finished_ = false, running_ = false, error_ = false;
  double real_start_ = 0, cpu_start_ = 0, real_ = 0, cpu_ = 0, manual_ = 0;
  std::int64_t items_ = 0, bytes_ = 0;
  std::string label_, error_message_;
};

namespace internal {

class Benchmark {
 public:
  Benchmark(std::string name, std::function<void(State&)> fn) : name_(std::move(name)), fn_(std::move(fn)) {}

  Benchmark* Arg(std::int64_t x) {
    args_.push_back({x});
    return this;
  }
  Benchmark* Args(const std::vector<std::int64_t>& xs) {
    args_.push_back(xs);
    return this;
  }
  Benchmark* Range(std::int64_t lo, std::int64_t hi) { return RangeMultiplier(range_multiplier_)->DoRange(lo, hi); }
  Benchmark* RangeMultiplier(int m) {
    range_multiplier_ = m < 2 ? 2 : m;
    return this;
  }
  Benchmark* DenseRange(std::int64_t lo, std::int64_t hi, int step = 1) {
    for (std::int64_t i = lo; i <= hi; i += step) args_.push_back({i});
    return this;
  }
  Benchmark* Ranges(const std::vector<std::pair<std::int64_t, std::int64_t>>& ranges) {
    std::vector<std::vector<std::int64_t>> per_dim;
    for (auto& r : ranges) per_dim.push_back(Expand(r.first, r.second));
    std::vector<std::vector<std::int64_t>> combos{{}};
    for (auto& values : per_dim) {
      std::vector<std::vector<std::int64_t>> next;
      for (auto& c : combos)
        for (auto v : values) {
          auto n = c;
          n.push_back(v);
          next.push_back(n);
        }
      combos = next;
    }
    args_.insert(args_.end(), combos.begin(), combos.end());
    return this;
  }
  Benchmark* Unit(TimeUnit u) {
    unit_ = u;
    return this;
  }
  Benchmark* Iterations(IterationCount n) {
    iterations_ = n;
    return this;
  }
  Benchmark* MinTime(double t) {
    min_time_ = t;
    return this;
  }
  Benchmark* Name(const std::string& n) {
    name_ = n;
    return this;
  }
  Benchmark* UseRealTime() {
    use_real_time_ = true;
    return this;
  }
  Benchmark* UseManualTime() {
    use_manual_time_ = true;
    return this;
  }
  Benchmark* Repetitions(int n) {
    repetitions_ = n;
    return this;
  }
  // Accepted for source compatibility, ignored.
  Benchmark* ArgName(const std::string&) { return this; }
  Benchmark* ArgNames(const std::vector<std::string>&) { return this; }
  Benchmark* Threads(int) { return this; }
  Benchmark* ThreadRange(int, int) { return this; }
  Benchmark* MeasureProcessCPUTime() { return this; }
  Benchmark* DisplayAggregatesOnly(bool = true) { return this; }
  Benchmark* ReportAggregatesOnly(bool = true) { return this; }
  template <class... Ts>
  Benchmark* Complexity(Ts...) {
    return this;
  }

  std::string name_;
  std::function<void(State&)> fn_;
  std::vector<std::vector<std::int64_t>> args_;
  TimeUnit unit_ = kNanosecond;
  IterationCount iterations_ = 0;
  double min_time_ = 0;
  int repetitions_ = 0;
  int range_multiplier_ = 8;
  bool use_real_time_ = false, use_manual_time_ = false;

 private:
  std::vector<std::int64_t> Expand(std::int64_t lo, std::int64_t hi) const {
    std::vector<std::int64_t> v{lo};
    for (std::int64_t i = 1; i < hi; i *= range_multiplier_)
      if (i > lo) v.push_back(i);
    if (hi != lo) v.push_back(hi);
    return v;
  }
  Benchmark* DoRange(std::int64_t lo, std::int64_t hi) {
    for (auto x : Expand(lo, hi)) args_.push_back({x});
    return this;
  }
};

inline std::vector<std::unique_ptr<Benchmark>>& Registry() {
  static std::vector<std::unique_ptr<Benchmark>> r;
  return r;
}

inline Benchmark* Register(std::string name, std::function<void(State&)> fn) {
  Registry().emplace_back(new Benchmark(std::move(name), std::move(fn)));
  return Registry().back().get();
}

struct Options {
  double min_time = 0.5;
  int repetitions = 1;
  bool json = false;
  std::string filter;  // substring match (Google Benchmark uses a regex)
};

struct Result {
  std::string name, run_name, run_type, aggregate_name, label, error;
  IterationCount iterations = 0;
  double real_time = 0, cpu_time = 0;  // per iteration, in the benchmark's unit
  TimeUnit unit = kNanosecond;
  int repetition_index = 0;
  std::map<std::string, double> counters;
};

inline double UnitMultiplier(TimeUnit u) {
  switch (u) {
    case kSecond: return 1;
    case kMillisecond: return 1e3;
    case kMicrosecond: return 1e6;
    default: return 1e9;
  }
}

inline const char* UnitName(TimeUnit u) {
  switch (u) {
    case kSecond: return "s";
    case kMillisecond: return "ms";
    case kMicrosecond: return "us";
    default: return "ns";
  }
}

inline Result RunOne(const Benchmark& b, const std::vector<std::int64_t>& args, const std::string& run_name,
                     const Options& opt) {
  const double min_time = b.min_time_ > 0 ? b.min_time_ : opt.min_time;
  IterationCount iters = b.iterations_ > 0 ? b.iterations_ : 1;
  for (;;) {
    State st(iters, args);
    b.fn_(st);
    const double measured = b.use_manual_time_ ? st.manual_time() : (b.use_real_time_ ? st.real_time() : st.cpu_time());
    const bool enough = b.iterations_ > 0 || st.error_occurred() || measured >= min_time ||
                        st.real_time() >= 5 * min_time || iters >= 1000000000;
    if (enough) {
      Result r;
      r.name = r.run_name = run_name;
      r.run_type = "iteration";
      r.iterations = iters;
      r.unit = b.unit_;
      const double m = UnitMultiplier(b.unit_) / static_cast<double>(iters);
      r.real_time = (b.use_manual_time_ ? st.manual_time() : st.real_time()) * m;
      r.cpu_time = st.cpu_time() * m;
      r.label = st.label();
      r.error = st.error_message();
      r.counters = st.counters;
      const double secs = b.use_real_time_ || b.use_manual_time_ ? r.real_time / UnitMultiplier(b.unit_) * iters
                                                                 : st.cpu_time();
      if (st.items_processed() && secs > 0) r.counters["items_per_second"] = st.items_processed() / secs;
      if (st.bytes_processed() && secs > 0) r.counters["bytes_per_second"] = st.bytes_processed() / secs;
      return r;
    }
    // Same growth strategy as Google Benchmark.
    double multiplier = min_time * 1.4 / std::max(measured, 1e-9);
    const bool significant = measured / min_time > 0.1;
    multiplier = significant ? multiplier : 10.0;
    if (multiplier <= 1.0) multiplier = 2.0;
    const double next = std::max(multiplier * static_cast<double>(iters), static_cast<double>(iters) + 1);
    iters = static_cast<IterationCount>(std::min(next, 1e9));
  }
}

inline std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

inline void PrintJson(const std::vector<Result>& results) {
  std::printf("{\n  \"context\": {\n    \"library\": \"quickbench\",\n    \"library_build_type\": \"release\"\n  },\n");
  std::printf("  \"benchmarks\": [\n");
  for (std::size_t i = 0; i < results.size(); ++i) {
    const Result& r = results[i];
    std::printf("    {\n      \"name\": \"%s\",\n      \"run_name\": \"%s\",\n      \"run_type\": \"%s\",\n",
                JsonEscape(r.name).c_str(), JsonEscape(r.run_name).c_str(), r.run_type.c_str());
    if (!r.aggregate_name.empty()) std::printf("      \"aggregate_name\": \"%s\",\n", r.aggregate_name.c_str());
    std::printf("      \"repetition_index\": %d,\n", r.repetition_index);
    if (!r.error.empty()) std::printf("      \"error_occurred\": true,\n      \"error_message\": \"%s\",\n", JsonEscape(r.error).c_str());
    if (!r.label.empty()) std::printf("      \"label\": \"%s\",\n", JsonEscape(r.label).c_str());
    for (auto& c : r.counters) std::printf("      \"%s\": %.17g,\n", JsonEscape(c.first).c_str(), c.second);
    std::printf("      \"iterations\": %lld,\n      \"real_time\": %.17g,\n      \"cpu_time\": %.17g,\n      \"time_unit\": \"%s\"\n    }%s\n",
                static_cast<long long>(r.iterations), r.real_time, r.cpu_time, UnitName(r.unit),
                i + 1 < results.size() ? "," : "");
  }
  std::printf("  ]\n}\n");
}

inline void PrintConsoleHeader() {
  std::printf("%-40s %15s %15s %12s\n", "Benchmark", "Time", "CPU", "Iterations");
  std::printf("%s\n", std::string(85, '-').c_str());
}

inline void PrintConsole(const Result& r) {
  if (!r.error.empty()) {
    std::printf("%-40s ERROR: %s\n", r.name.c_str(), r.error.c_str());
    return;
  }
  std::printf("%-40s %12.1f %-2s %12.1f %-2s %12lld %s\n", r.name.c_str(), r.real_time, UnitName(r.unit), r.cpu_time,
              UnitName(r.unit), static_cast<long long>(r.iterations), r.label.c_str());
  std::fflush(stdout);
}

inline Options ParseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](const char* flag) -> const char* {
      std::size_t n = std::strlen(flag);
      return a.compare(0, n, flag) == 0 ? a.c_str() + n : nullptr;
    };
    if (const char* v = value("--benchmark_min_time=")) {
      o.min_time = std::atof(v);  // accepts "0.1" and "0.1s"
    } else if (const char* v = value("--benchmark_format=")) {
      o.json = std::strcmp(v, "json") == 0;
    } else if (const char* v = value("--benchmark_filter=")) {
      o.filter = v;
    } else if (const char* v = value("--benchmark_repetitions=")) {
      o.repetitions = std::max(1, std::atoi(v));
    } else if (a.rfind("--benchmark_", 0) == 0) {
      std::fprintf(stderr, "quickbench: ignoring unsupported flag %s\n", a.c_str());
    }
  }
  if (o.min_time <= 0) o.min_time = 0.5;
  return o;
}

inline void AddAggregates(std::vector<Result>& out, const std::vector<Result>& reps) {
  const std::size_t n = reps.size();
  const char* names[] = {"mean", "median", "stddev"};
  for (const char* agg : names) {
    Result r = reps.front();
    r.name = r.run_name + "_" + agg;
    r.run_type = "aggregate";
    r.aggregate_name = agg;
    r.counters.clear();
    std::vector<double> real, cpu;
    for (auto& x : reps) {
      real.push_back(x.real_time);
      cpu.push_back(x.cpu_time);
    }
    auto mean = [&](const std::vector<double>& v) {
      double s = 0;
      for (double x : v) s += x;
      return s / static_cast<double>(n);
    };
    auto median = [](std::vector<double> v) {
      std::sort(v.begin(), v.end());
      const std::size_t m = v.size() / 2;
      return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
    };
    auto stddev = [&](const std::vector<double>& v) {
      if (n < 2) return 0.0;
      const double mu = mean(v);
      double s = 0;
      for (double x : v) s += (x - mu) * (x - mu);
      return std::sqrt(s / static_cast<double>(n - 1));
    };
    const std::string a = agg;
    if (a == "mean") { r.real_time = mean(real); r.cpu_time = mean(cpu); }
    if (a == "median") { r.real_time = median(real); r.cpu_time = median(cpu); }
    if (a == "stddev") { r.real_time = stddev(real); r.cpu_time = stddev(cpu); }
    r.iterations = static_cast<IterationCount>(n);
    out.push_back(r);
  }
}

}  // namespace internal

inline int RunSpecifiedBenchmarks(int argc, char** argv) {
  using namespace internal;
  const Options opt = ParseArgs(argc, argv);
  std::vector<Result> results;
  if (!opt.json) PrintConsoleHeader();
  for (auto& bp : Registry()) {
    const Benchmark& b = *bp;
    std::vector<std::vector<std::int64_t>> arg_sets = b.args_;
    if (arg_sets.empty()) arg_sets.push_back({});
    for (auto& args : arg_sets) {
      std::string run_name = b.name_;
      for (auto x : args) run_name += "/" + std::to_string(x);
      if (!opt.filter.empty() && opt.filter != "." && opt.filter != "all" &&
          run_name.find(opt.filter) == std::string::npos)
        continue;
      const int reps = b.repetitions_ > 0 ? b.repetitions_ : opt.repetitions;
      std::vector<Result> rep_results;
      for (int rep = 0; rep < reps; ++rep) {
        Result r = RunOne(b, args, run_name, opt);
        r.repetition_index = rep;
        if (!opt.json) PrintConsole(r);
        rep_results.push_back(r);
      }
      results.insert(results.end(), rep_results.begin(), rep_results.end());
      if (reps > 1) {
        const std::size_t before = results.size();
        AddAggregates(results, rep_results);
        if (!opt.json)
          for (std::size_t i = before; i < results.size(); ++i) PrintConsole(results[i]);
      }
    }
  }
  if (opt.json) PrintJson(results);
  return 0;
}

inline int RunSpecifiedBenchmarks() {
  char name[] = "quickbench";
  char* argv[] = {name, nullptr};
  return RunSpecifiedBenchmarks(1, argv);
}

inline void Initialize(int*, char**) {}
inline bool ReportUnrecognizedArguments(int, char**) { return false; }
inline void Shutdown() {}

}  // namespace benchmark

#define QB_CONCAT_(a, b) a##b
#define QB_CONCAT(a, b) QB_CONCAT_(a, b)
#define QB_UNIQUE(prefix) QB_CONCAT(prefix, __COUNTER__)

#define BENCHMARK(...)                                                                         \
  static ::benchmark::internal::Benchmark* QB_UNIQUE(qb_bench_) [[maybe_unused]] = \
      ::benchmark::internal::Register(#__VA_ARGS__, __VA_ARGS__)

#define BENCHMARK_TEMPLATE(fn, ...)                                                            \
  static ::benchmark::internal::Benchmark* QB_UNIQUE(qb_bench_) [[maybe_unused]] = \
      ::benchmark::internal::Register(#fn "<" #__VA_ARGS__ ">", fn<__VA_ARGS__>)
#define BENCHMARK_TEMPLATE1(fn, a) BENCHMARK_TEMPLATE(fn, a)
#define BENCHMARK_TEMPLATE2(fn, a, b) BENCHMARK_TEMPLATE(fn, a, b)

#define BENCHMARK_CAPTURE(fn, name, ...)                                                       \
  static ::benchmark::internal::Benchmark* QB_UNIQUE(qb_bench_) [[maybe_unused]] = \
      ::benchmark::internal::Register(#fn "/" #name,                                           \
                                      [](::benchmark::State& st) { fn(st, __VA_ARGS__); })

#define BENCHMARK_MAIN()                                        \
  int main(int argc, char** argv) {                             \
    ::benchmark::Initialize(&argc, argv);                       \
    return ::benchmark::RunSpecifiedBenchmarks(argc, argv);     \
  }                                                             \
  int main(int, char**)
