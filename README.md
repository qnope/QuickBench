# QuickBench

A static, [Quick Bench](https://quick-bench.com)–like C++ micro-benchmarking site that runs entirely on GitHub Pages.

Write [Google Benchmark](https://github.com/google/benchmark) functions in the editor, pick a compiler and flags, and hit **Run benchmark** (or `Ctrl+Enter`). The code is compiled and executed through the public [Compiler Explorer API](https://github.com/compiler-explorer/compiler-explorer/blob/main/docs/API.md), and the results are plotted relative to a `Noop` baseline, like Quick Bench does.

## Features

- C++ editor (CodeMirror) with GCC / Clang selection, standard, optimization level and extra flags
- Compilation and execution on godbolt.org with Google Benchmark linked in
- Bar chart normalized to a `Noop` benchmark, results table and raw build/run output
- Share links (the whole state is stored in the URL hash) and local autosave
- "Open in CE" button to inspect the same code and assembly on Compiler Explorer

## How it works

There is no backend. The page calls:

- `GET /api/compilers/c++` and `GET /api/libraries/c++` to list execution-capable x86-64 compilers and the latest Google Benchmark version;
- `POST /api/compiler/{id}/compile` with `filters.execute = true` and `--benchmark_format=json` to build, run and read the results.

The user code is wrapped automatically: `#include <benchmark/benchmark.h>` (plus a few common headers), a `Noop` benchmark and `BENCHMARK_MAIN()` are added unless already present.

## Limits

- Depends on godbolt.org being available and keeping its API stable.
- Executions are short (a few seconds); keep benchmarks small or lower `--benchmark_min_time`.
- Machines are shared, so absolute timings are noisy. Compare benchmarks relative to each other.

## Deploying on GitHub Pages

Push these files to a repository, then in **Settings → Pages** choose *Deploy from a branch*, branch `main`, folder `/ (root)`. The site is then served at `https://<user>.github.io/<repo>/`.

To try it locally: `python3 -m http.server` in this folder, then open http://localhost:8000.

## Third-party code

`vendor/` contains [CodeMirror 5](https://codemirror.net/5/) (MIT) and [Chart.js](https://www.chartjs.org/) (MIT), vendored so the site needs no CDN.
