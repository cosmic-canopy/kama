// Render the home page's performance section from docs/benchmarks/results.json — the SAME file
// `bench/run report` writes. The numbers on the marketing page are therefore the measured numbers,
// regenerated with the benchmark; there is no hand-maintained second copy to drift.
//
// Form: small multiples (one mini bar panel per workload, each scaled to its own max). The workloads
// span ~50x in absolute time, so a single shared axis would squash the fast ones into invisibility —
// and a second axis is never the answer. Every bar is directly labelled, so the panels need no axis
// and no hover layer: there is no value hidden behind an interaction.
//
// Colour: kama wears the site's ember accent, every other language a neutral. That is identity, not
// rank — kama is the subject of the page, and the ordering is fixed (by measured time) regardless.
// Each bar carries its language name as text, so identity is never colour-alone.

import { readFileSync } from 'node:fs';
import path from 'node:path';

// The compiled/AOT cohort. C#/Java/Lua/Python are deliberately left out of the GRAPH (not the data):
// they run 5-40x slower here, so including them would compress this whole cluster to a single pixel.
// The full table on /docs/benchmarks/ has every language; the prose below links to it and says so.
const LANGS = ['kama', 'c', 'cpp', 'rust', 'go'];
const LABEL = { kama: 'kama', c: 'C', cpp: 'C++', rust: 'Rust', go: 'Go' };

// What each workload measures, in a few words — a bar chart with no explanation is a decoration.
const BLURB = {
  fib: 'recursive calls',
  pi: 'float throughput',
  collatz: 'integer + branches',
  dispatch: 'virtual dispatch',
  alloc: 'alloc / RAII churn',
  fnptr: 'indirect calls',
  map: 'idiomatic hash maps',
  map_kernel: 'hash map, one algorithm',
  math: 'vector / matrix math',
};

const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

export function benchSection(root) {
  const data = JSON.parse(readFileSync(path.join(root, 'docs/benchmarks/results.json'), 'utf8'));
  const at = (lang, workload, field) => {
    const r = data.rows.find(r => r.track === 'native' && r.lang === lang && r.workload === workload);
    const v = r && r[field];
    return v && v !== 'NA' ? Number(v) : null;
  };

  // Workload order follows the report; only those with a kama number are plotted.
  const workloads = [...new Set(data.rows.filter(r => r.track === 'native').map(r => r.workload))]
    .filter(w => at('kama', w, 'time_ms') !== null);

  const panels = workloads.map(w => {
    const series = LANGS
      .map(l => ({ lang: l, ms: at(l, w, 'time_ms') }))
      .filter(s => s.ms !== null)
      .sort((a, b) => a.ms - b.ms);
    const max = Math.max(...series.map(s => s.ms));
    const rows = series.map(s => {
      const pct = (s.ms / max) * 100;
      const isKama = s.lang === 'kama';
      return `<div class="bench-row${isKama ? ' is-kama' : ''}">
          <span class="bench-lang">${esc(LABEL[s.lang])}</span>
          <span class="bench-track"><span class="bench-fill" style="width:${pct.toFixed(1)}%"></span></span>
          <span class="bench-val">${s.ms.toFixed(2)}</span>
        </div>`;
    }).join('\n');
    return `<figure class="bench-panel">
        <figcaption><b>${esc(w)}</b><span>${esc(BLURB[w] || '')}</span></figcaption>
        ${rows}
      </figure>`;
  }).join('\n');

  // Two headline figures that a time chart cannot show, both straight from the same rows.
  // Floor, matching report.py's `int(v)//1024` — the site must not disagree with its own table.
  const rss = l => Math.floor((at(l, 'collatz', 'rss_kb') || 0) / 1024);
  const sizeKB = l => {
    for (const w of workloads) { const b = at(l, w, 'size_bytes'); if (b) return b / 1024; }
    return null;
  };
  const javaRss = (() => {
    const r = data.rows.find(r => r.track === 'native' && r.lang === 'java' && r.workload === 'collatz');
    return r && r.rss_kb !== 'NA' ? Math.floor(Number(r.rss_kb) / 1024) : null;
  })();
  const goSize = (() => {
    for (const w of workloads) {
      const r = data.rows.find(r => r.track === 'native' && r.lang === 'go' && r.workload === w);
      if (r && r.size_bytes && r.size_bytes !== '0') return Number(r.size_bytes) / 1024;
    }
    return null;
  })();

  const env = data.env || {};
  return { panels, kamaRss: rss('kama'), javaRss, kamaSize: sizeKB('kama'), goSize, env };
}
