'use strict';

// Everything runs in the browser: compilation and execution are delegated to
// the public Compiler Explorer API, which allows cross-origin requests.
const CE_API = 'https://godbolt.org/api';
const BENCH_LIB_ID = 'benchmark';
const NOOP_NAME = 'Noop';
const STORAGE_KEY = 'quickbench.state';

const DEFAULT_SOURCE = `static void StringCreation(benchmark::State& state) {
  // Code inside this loop is measured repeatedly
  for (auto _ : state) {
    std::string created_string("hello");
    // Make sure the variable is not optimized away by compiler
    benchmark::DoNotOptimize(created_string);
  }
}
// Register the function as a benchmark
BENCHMARK(StringCreation);

static void StringCopy(benchmark::State& state) {
  // Code before the loop is not measured
  std::string x = "hello";
  for (auto _ : state) {
    std::string copy(x);
    benchmark::DoNotOptimize(copy);
  }
}
BENCHMARK(StringCopy);
`;

const $ = (id) => document.getElementById(id);

const ui = {
  compiler: $('compiler'),
  std: $('std'),
  opt: $('opt'),
  flags: $('flags'),
  runargs: $('runargs'),
  run: $('run'),
  share: $('share'),
  godbolt: $('godbolt'),
  status: $('status'),
  output: $('output'),
  summary: $('summary'),
  chartEmpty: $('chart-empty'),
  tableBody: document.querySelector('#table tbody'),
};

let benchLib = null;   // { id, version } of Google Benchmark on Compiler Explorer
let chart = null;
let pendingCompiler = null;

const editor = CodeMirror($('editor'), {
  value: DEFAULT_SOURCE,
  mode: 'text/x-c++src',
  theme: 'material-darker',
  lineNumbers: true,
  indentUnit: 2,
  tabSize: 2,
  matchBrackets: true,
  autoCloseBrackets: true,
  extraKeys: {
    'Ctrl-Enter': () => runBenchmark(),
    'Cmd-Enter': () => runBenchmark(),
  },
});

// ---------------------------------------------------------------- state ----

function currentState() {
  return {
    src: editor.getValue(),
    compiler: ui.compiler.value || pendingCompiler,
    std: ui.std.value,
    opt: ui.opt.value,
    flags: ui.flags.value,
    runargs: ui.runargs.value,
  };
}

function applyState(s) {
  if (!s) return;
  if (typeof s.src === 'string') editor.setValue(s.src);
  if (s.std) ui.std.value = s.std;
  if (s.opt) ui.opt.value = s.opt;
  if (typeof s.flags === 'string') ui.flags.value = s.flags;
  if (typeof s.runargs === 'string') ui.runargs.value = s.runargs;
  if (s.compiler) pendingCompiler = s.compiler;
}

function encodeState(s) {
  const bytes = new TextEncoder().encode(JSON.stringify(s));
  let bin = '';
  bytes.forEach((b) => { bin += String.fromCharCode(b); });
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function decodeState(str) {
  const b64 = str.replace(/-/g, '+').replace(/_/g, '/');
  const bin = atob(b64);
  const bytes = Uint8Array.from(bin, (c) => c.charCodeAt(0));
  return JSON.parse(new TextDecoder().decode(bytes));
}

function loadInitialState() {
  const m = location.hash.match(/^#s=(.+)$/);
  if (m) {
    try { return decodeState(m[1]); } catch (e) { console.warn('Bad share link', e); }
  }
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) return JSON.parse(saved);
  } catch (e) { /* storage unavailable */ }
  return null;
}

function saveState() {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(currentState())); } catch (e) { /* ignore */ }
}

// ------------------------------------------------------ compiler explorer ----

async function ceGet(path) {
  const res = await fetch(CE_API + path, { headers: { Accept: 'application/json' } });
  if (!res.ok) throw new Error(`GET ${path} failed: HTTP ${res.status}`);
  return res.json();
}

function versionKey(v) {
  return (v.version || v.id).split(/[^0-9]+/).filter(Boolean).map(Number);
}

function compareVersions(a, b) {
  const ka = versionKey(a), kb = versionKey(b);
  for (let i = 0; i < Math.max(ka.length, kb.length); i++) {
    const d = (ka[i] || 0) - (kb[i] || 0);
    if (d) return d;
  }
  return 0;
}

async function loadBenchmarkLibrary() {
  const libs = await ceGet('/libraries/c++');
  const lib = libs.find((l) => l.id === BENCH_LIB_ID);
  if (!lib || !lib.versions || !lib.versions.length) {
    throw new Error('Google Benchmark is not available on Compiler Explorer');
  }
  // Prefer the newest released version over trunk, which can be unstable.
  const released = lib.versions.filter((v) => !/trunk/i.test(v.id));
  const pool = released.length ? released : lib.versions;
  const best = pool.slice().sort(compareVersions).pop();
  benchLib = { id: lib.id, version: best.id, label: best.version || best.id };
}

function isUsableCompiler(c) {
  if (c.supportsExecute === false) return false;
  if (c.instructionSet && c.instructionSet !== 'amd64') return false;
  return /^(x86-64 )?(gcc|clang) /i.test(c.name) && !/(assert|trunk|cross|cuda|hip|rocm|ibm|bpf|wasm|cl430)/i.test(c.name);
}

async function loadCompilers() {
  const all = await ceGet('/compilers/c++?fields=id,name,semver,compilerType,instructionSet,supportsExecute');
  const list = all.filter(isUsableCompiler);
  const family = (c) => (/clang/i.test(c.name) ? 'clang' : 'gcc');
  const groups = { gcc: [], clang: [] };
  list.forEach((c) => groups[family(c)].push(c));
  const bySemverDesc = (a, b) => compareVersions({ id: b.semver || b.name }, { id: a.semver || a.name });

  ui.compiler.innerHTML = '';
  for (const [name, items] of Object.entries(groups)) {
    if (!items.length) continue;
    const og = document.createElement('optgroup');
    og.label = name.toUpperCase();
    items.sort(bySemverDesc).slice(0, 15).forEach((c) => {
      const o = document.createElement('option');
      o.value = c.id;
      o.textContent = c.name;
      og.appendChild(o);
    });
    ui.compiler.appendChild(og);
  }
  if (pendingCompiler && list.some((c) => c.id === pendingCompiler)) {
    ui.compiler.value = pendingCompiler;
  } else if (groups.clang.length) {
    ui.compiler.value = groups.clang.sort(bySemverDesc)[0].id;
  }
  pendingCompiler = null;
}

function buildSource(userSrc) {
  let src = '';
  if (!/#\s*include\s*<benchmark\/benchmark\.h>/.test(userSrc)) {
    src += '#include <benchmark/benchmark.h>\n#include <string>\n#include <vector>\n#include <algorithm>\n#include <numeric>\n#include <memory>\n#include <map>\n#include <unordered_map>\n';
  }
  src += '#line 1\n' + userSrc + '\n';
  // Baseline used to normalize the results, as Quick Bench does.
  src += `\nstatic void ${NOOP_NAME}(benchmark::State& state) {\n  for (auto _ : state) benchmark::DoNotOptimize(0);\n}\nBENCHMARK(${NOOP_NAME});\n`;
  if (!/BENCHMARK_MAIN\s*\(/.test(userSrc) && !/\bint\s+main\s*\(/.test(userSrc)) {
    src += 'BENCHMARK_MAIN();\n';
  }
  return src;
}

function compilerArgs(s) {
  return [`-std=${s.std}`, s.opt, '-pthread', s.flags].filter(Boolean).join(' ');
}

function splitArgs(str) {
  return (str.match(/(?:[^\s"']+|"[^"]*"|'[^']*')+/g) || []).map((a) => a.replace(/^["']|["']$/g, ''));
}

async function compileAndRun(s) {
  const body = {
    source: buildSource(s.src),
    lang: 'c++',
    allowStoreCodeDebug: true,
    options: {
      userArguments: compilerArgs(s),
      executeParameters: {
        args: ['--benchmark_format=json', ...splitArgs(s.runargs)],
        stdin: '',
      },
      compilerOptions: { executorRequest: true, skipAsm: true },
      filters: { execute: true },
      tools: [],
      libraries: [{ id: benchLib.id, version: benchLib.version }],
    },
  };
  const res = await fetch(`${CE_API}/compiler/${encodeURIComponent(s.compiler)}/compile`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`Compiler Explorer answered HTTP ${res.status}`);
  return res.json();
}

const joinLines = (arr) => (arr || []).map((l) => l.text).join('\n');

// -------------------------------------------------------------- results ----

function parseBenchmarkJson(stdout) {
  const start = stdout.indexOf('{');
  const end = stdout.lastIndexOf('}');
  if (start < 0 || end < start) return null;
  try { return JSON.parse(stdout.slice(start, end + 1)); } catch (e) { return null; }
}

function formatTime(v, unit) {
  return `${v.toFixed(v < 10 ? 2 : 1)} ${unit}`;
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function renderResults(data) {
  const runs = (data.benchmarks || []).filter((b) => !b.run_type || b.run_type === 'iteration' || b.aggregate_name === 'mean');
  const noop = runs.find((b) => b.name === NOOP_NAME || b.run_name === NOOP_NAME);
  const userRuns = runs.filter((b) => b !== noop && !(b.run_name || b.name).startsWith(NOOP_NAME));
  const base = noop ? noop.cpu_time : null;
  const ratio = (b) => (base ? b.cpu_time / base : b.cpu_time);

  ui.tableBody.innerHTML = '';
  runs.forEach((b) => {
    const tr = document.createElement('tr');
    [b.name, formatTime(b.cpu_time, b.time_unit), formatTime(b.real_time, b.time_unit),
      String(b.iterations), base ? ratio(b).toFixed(1) : '–'].forEach((txt) => {
      const td = document.createElement('td');
      td.textContent = txt;
      tr.appendChild(td);
    });
    ui.tableBody.appendChild(tr);
  });

  const palette = [cssVar('--accent'), cssVar('--accent-2'), '#4cc38a', '#c084fc', '#f472b6', '#facc15', '#38bdf8', '#fb7185'];
  const labels = userRuns.map((b) => b.name);
  const values = userRuns.map(ratio);
  const yTitle = base ? 'CPU time / Noop time (lower is faster)' : `CPU time (${userRuns[0] ? userRuns[0].time_unit : ''})`;

  ui.chartEmpty.style.display = userRuns.length ? 'none' : '';
  if (chart) chart.destroy();
  chart = new Chart($('chart'), {
    type: 'bar',
    data: {
      labels,
      datasets: [{
        data: values,
        backgroundColor: labels.map((_, i) => palette[i % palette.length]),
        borderRadius: 4,
        maxBarThickness: 80,
      }],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 300 },
      plugins: {
        legend: { display: false },
        tooltip: {
          callbacks: {
            label: (ctx) => {
              const b = userRuns[ctx.dataIndex];
              return [`CPU: ${formatTime(b.cpu_time, b.time_unit)}`, base ? `× Noop: ${ctx.parsed.y.toFixed(1)}` : ''].filter(Boolean);
            },
          },
        },
      },
      scales: {
        x: { ticks: { color: cssVar('--text') }, grid: { display: false } },
        y: {
          beginAtZero: true,
          title: { display: true, text: yTitle, color: cssVar('--muted') },
          ticks: { color: cssVar('--muted') },
          grid: { color: cssVar('--border') },
        },
      },
    },
  });

  if (userRuns.length >= 2) {
    const sorted = userRuns.slice().sort((a, b) => a.cpu_time - b.cpu_time);
    const fastest = sorted[0];
    ui.summary.innerHTML = sorted.slice(1).map((b) =>
      `<strong>${escapeHtml(fastest.name)}</strong> is <strong>${(b.cpu_time / fastest.cpu_time).toFixed(1)}×</strong> faster than ${escapeHtml(b.name)}`).join('<br>');
  } else {
    ui.summary.textContent = '';
  }
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

// ------------------------------------------------------------------- ui ----

function setStatus(text, kind) {
  ui.status.textContent = text;
  ui.status.className = 'status' + (kind ? ' ' + kind : '');
}

function showTab(name) {
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.dataset.tab === name));
  document.querySelectorAll('.tab-body').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  if (name === 'chart' && chart) chart.resize();
}

function setOutput(sections) {
  ui.output.innerHTML = '';
  sections.filter((s) => s.text).forEach((s) => {
    const h = document.createElement('div');
    h.textContent = `── ${s.title} ──`;
    const p = document.createElement('div');
    p.textContent = s.text;
    if (s.error) p.className = 'err';
    ui.output.append(h, p, document.createElement('br'));
  });
}

async function runBenchmark() {
  if (ui.run.disabled) return;
  const s = currentState();
  saveState();
  ui.run.disabled = true;
  setStatus('Compiling and running on godbolt.org…', 'running');
  const t0 = performance.now();
  try {
    const r = await compileAndRun(s);
    const build = r.buildResult || {};
    const stdout = joinLines(r.stdout);
    const stderr = joinLines(r.stderr);
    const buildOut = [joinLines(build.stdout), joinLines(build.stderr)].filter(Boolean).join('\n');
    setOutput([
      { title: 'Build', text: buildOut, error: build.code !== 0 },
      { title: 'stdout', text: stdout },
      { title: 'stderr', text: stderr, error: r.code !== 0 },
    ]);

    if (build.code && build.code !== 0) {
      setStatus('Compilation failed', 'error');
      showTab('output');
      return;
    }
    if (!r.didExecute) {
      setStatus('Program did not execute', 'error');
      showTab('output');
      return;
    }
    const data = parseBenchmarkJson(stdout);
    if (!data || !data.benchmarks) {
      setStatus(r.timedOut ? 'Execution timed out' : `Execution failed (exit ${r.code})`, 'error');
      showTab('output');
      return;
    }
    renderResults(data);
    const secs = ((performance.now() - t0) / 1000).toFixed(1);
    setStatus(`Done in ${secs}s · ${benchLib.label ? 'benchmark ' + benchLib.label : ''}`, 'ok');
    showTab('chart');
  } catch (e) {
    setStatus(e.message, 'error');
    setOutput([{ title: 'Error', text: String(e.stack || e), error: true }]);
    showTab('output');
  } finally {
    ui.run.disabled = false;
  }
}

function shareLink() {
  const url = `${location.origin}${location.pathname}#s=${encodeState(currentState())}`;
  history.replaceState(null, '', url);
  const done = () => setStatus('Link copied to clipboard', 'ok');
  if (navigator.clipboard) {
    navigator.clipboard.writeText(url).then(done, () => setStatus('Link is in the address bar', 'ok'));
  } else {
    setStatus('Link is in the address bar', 'ok');
  }
}

function openInCompilerExplorer() {
  const s = currentState();
  const libs = benchLib ? [{ name: benchLib.id, ver: benchLib.version }] : [];
  const options = compilerArgs(s);
  const state = {
    sessions: [{
      id: 1,
      language: 'c++',
      source: buildSource(s.src),
      compilers: [{ id: s.compiler, options, libs }],
      executors: [{
        arguments: ['--benchmark_format=console', ...splitArgs(s.runargs)].join(' '),
        compiler: { id: s.compiler, options, libs },
      }],
    }],
  };
  const b64 = btoa(String.fromCharCode(...new TextEncoder().encode(JSON.stringify(state))));
  window.open(`https://godbolt.org/clientstate/${encodeURIComponent(b64)}`, '_blank', 'noopener');
}

document.querySelectorAll('.tab').forEach((t) => t.addEventListener('click', () => showTab(t.dataset.tab)));
ui.run.addEventListener('click', runBenchmark);
ui.share.addEventListener('click', shareLink);
ui.godbolt.addEventListener('click', openInCompilerExplorer);
editor.on('change', () => { clearTimeout(editor._saveTimer); editor._saveTimer = setTimeout(saveState, 500); });
[ui.compiler, ui.std, ui.opt, ui.flags, ui.runargs].forEach((el) => el.addEventListener('change', saveState));

async function init() {
  applyState(loadInitialState());
  setStatus('Loading compilers from godbolt.org…', 'running');
  try {
    await Promise.all([loadCompilers(), loadBenchmarkLibrary()]);
    ui.run.disabled = false;
    setStatus(`Ready · Google Benchmark ${benchLib.label}`, 'ok');
  } catch (e) {
    setStatus('Could not reach Compiler Explorer', 'error');
    setOutput([{ title: 'Error', text: String(e.message || e), error: true }]);
    showTab('output');
  }
}

init();
