// Build a kama registry's human pages — registry.kama-lang.org's — from the registry tree itself.
//
//   node tools/site/registry.mjs --registry <dir> --out <dir> [--origin <url>]
//
// <dir> is a registry as `kama publish` writes it (docs/packages.md § The registry is a static file tree):
// `<name>/index.json` per package and `catalog.json`. Everything in it is copied to <out> byte for byte — the
// files the toolchain reads stay exactly what was published — and beside them this writes the pages people
// read: `index.html` (every package, searchable by the rule `kama pkg search` applies to the same catalog), one
// page per package at `<name>/index.html` (install line, versions, dependencies, the README), `sitemap.xml`,
// and the main site's stylesheet, fonts and icons, so a registry page looks like kama-lang.org.
//
// A README comes out of the version's tarball: read beside its index when the registry holds tarballs (a
// `file://` registry does), else fetched from <origin> and checked against the recorded integrity before a byte
// of it is shown. It is someone else's text, so its HTML is shown and never run, and a link is kept only when it
// is `https:`/`http:`/`mailto:` (a relative one names a file this host does not serve). Publish dates come
// from the registry's git history — the commit that added each version — and are left out off a checkout.

import { readFileSync, writeFileSync, mkdirSync, rmSync, cpSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { Marked } from 'marked';
import { highlight, kamaWords } from './highlight.mjs';
import { shell, escAttr, setVersion, REGISTRY_SITE } from './layout.mjs';
import { slugify } from './render.mjs';

const root = fileURLToPath(new URL('../..', import.meta.url));
const DEFAULT_ORIGIN = 'https://registry.kama-lang.org';

// ---------------------------------------------------------------- arguments

const args = process.argv.slice(2);
const opt = name => { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : null; };
const regDir = opt('--registry'), outDir = opt('--out');
const origin = (opt('--origin') || DEFAULT_ORIGIN).replace(/\/+$/, '');
if (!regDir || !outDir) {
  console.error('usage: node tools/site/registry.mjs --registry <dir> --out <dir> [--origin <url>]');
  process.exit(2);
}
const fail = msg => { console.error(`registry site: ${msg}`); process.exit(1); };

const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const asciiLower = s => String(s).replace(/[A-Z]/g, c => c.toLowerCase());
const readJson = file => {
  try { return JSON.parse(readFileSync(file, 'utf8')); }
  catch (e) { fail(`${file}: ${e.message}`); }
};
const semver = v => { const m = /^(\d+)\.(\d+)\.(\d+)$/.exec(v || ''); return m ? m.slice(1).map(Number) : null; };
const newerFirst = (a, b) => {
  const x = semver(a.version), y = semver(b.version);
  if (!x || !y) return x ? -1 : y ? 1 : 0;
  return y[0] - x[0] || y[1] - x[1] || y[2] - x[2];
};

// ---------------------------------------------------------------- the registry

const catalogPath = path.join(regDir, 'catalog.json');
if (!existsSync(catalogPath)) fail(`${catalogPath} is missing — \`kama publish\` writes it (kama 0.9.523 and later)`);
const catalog = readJson(catalogPath).packages;
if (!Array.isArray(catalog)) fail(`${catalogPath} has no \`packages\` array`);

const packages = catalog.map(entry => {
  const indexPath = path.join(regDir, entry.name, 'index.json');
  if (!existsSync(indexPath)) fail(`catalog.json lists ${entry.name}, which has no ${entry.name}/index.json`);
  const index = readJson(indexPath);
  const versions = (index.versions || []).slice().sort(newerFirst);
  if (!versions.length || versions[0].version !== entry.version)
    fail(`catalog.json says ${entry.name} is at ${entry.version}, but its index's highest version is ${versions[0]?.version}`);
  return { ...entry, versions };
});
const byName = new Map(packages.map(p => [p.name, p]));

// When each version landed: the oldest commit whose diff added its line to the index (git's pickaxe).
function publishedOn(name, version) {
  try {
    const out = execFileSync('git', ['-C', regDir, 'log', '--format=%cs', `-S"version": "${version}"`, '--',
                                     path.join(name, 'index.json')], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
    const lines = out.trim().split('\n').filter(Boolean);
    return lines.length ? lines[lines.length - 1] : '';
  } catch { return ''; }
}

// ---------------------------------------------------------------- READMEs

// The README at the archive's top level (`<package>/README.md`): kama writes plain ustar, one directory deep.
function readmeFromTarball(tgz) {
  const tar = gunzipSync(tgz);
  const field = (h, at, len) => { const b = h.subarray(at, at + len); const z = b.indexOf(0); return b.subarray(0, z < 0 ? len : z).toString('utf8'); };
  for (let off = 0; off + 512 <= tar.length;) {
    const h = tar.subarray(off, off + 512);
    if (h.every(b => b === 0)) break;
    const name = field(h, 0, 100), prefix = field(h, 345, 155);
    const size = parseInt(field(h, 124, 12).trim() || '0', 8);
    const type = h[156] === 0 ? '0' : String.fromCharCode(h[156]);
    const full = prefix ? `${prefix}/${name}` : name;
    if (type === '0' && /^[^/]+\/readme\.md$/i.test(full)) return tar.subarray(off + 512, off + 512 + size).toString('utf8');
    off += 512 + Math.ceil(size / 512) * 512;
  }
  return null;
}

async function tarballOf(version) {
  const local = path.join(regDir, version.tarball);
  if (!/^[a-z]+:/i.test(version.tarball) && existsSync(local)) return readFileSync(local);
  const url = new URL(version.tarball, origin + '/');
  const res = await fetch(url, { signal: AbortSignal.timeout(30000) });
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  return Buffer.from(await res.arrayBuffer());
}

async function readmeOf(pkg) {
  const v = pkg.versions[0];
  try {
    const tgz = await tarballOf(v);
    const got = 'sha256-' + createHash('sha256').update(tgz).digest('hex');
    if (got !== v.integrity) throw new Error(`the tarball is ${got}, but the index records ${v.integrity}`);
    return readmeFromTarball(tgz);
  } catch (e) {
    // The pages are for people; the index files beside them are what the toolchain needs, and an outage of the
    // tarball host must not hold those back. Said loudly, so the next deploy is known to be needed.
    console.error(`registry site: WARNING — no README for ${pkg.name}@${v.version}: ${e.message}`);
    return null;
  }
}

const words = kamaWords(root);

function renderReadme(md) {
  const seen = new Map();
  const marked = new Marked({
    gfm: true,
    renderer: {
      // The page's own <h1> is the package name, so the README's headings sit one level below it.
      heading(token) {
        const text = this.parser.parseInline(token.tokens);
        const base = slugify(token.text) || 'section';
        const n = seen.get(base) ?? 0;
        seen.set(base, n + 1);
        const depth = Math.min(token.depth + 1, 6);
        return `<h${depth} id="${n ? `${base}-${n}` : base}">${text}</h${depth}>\n`;
      },
      html(token) { return token.block ? `<pre class="code"><code>${esc(token.raw)}</code></pre>\n` : esc(token.raw); },
      link(token) {
        const text = this.parser.parseInline(token.tokens);
        if (/^(https?:|mailto:)/i.test(token.href)) return `<a href="${escAttr(token.href)}" rel="noopener nofollow">${text}</a>`;
        if (token.href.startsWith('#')) return `<a href="${escAttr(token.href)}">${text}</a>`;
        return text;
      },
      image(token) {
        return /^https:/i.test(token.href)
          ? `<img src="${escAttr(token.href)}" alt="${escAttr(token.text)}" loading="lazy">`
          : esc(token.text);
      },
      code(token) {
        const lang = (token.lang || '').split(/\s+/)[0];
        return `<pre class="code"${lang ? ` data-lang="${escAttr(lang)}"` : ''}><code>${highlight(token.text, lang, words)}</code></pre>\n`;
      },
    },
  });
  return marked.parse(md);
}

// ---------------------------------------------------------------- pages

const site = REGISTRY_SITE(origin);
setVersion(readFileSync(path.join(root, 'VERSION'), 'utf8').trim());
const installLine = (name, version) =>
  `kama pkg add kama.json ${name} --version ^${version}` + (origin === DEFAULT_ORIGIN ? '' : ` --registry ${origin}`);
const pageUrl = name => `/${name}/`;
const keywordLinks = kws => (kws || []).map(k => `<a href="/?q=${encodeURIComponent(k)}">${esc(k)}</a>`).join(' ');

function listItem(p) {
  return `    <li data-name="${escAttr(p.name)}" data-description="${escAttr(p.description || '')}" data-keywords="${escAttr((p.keywords || []).join(' '))}">
      <a class="card" href="${pageUrl(p.name)}">
        <h3>${esc(p.name)} <span class="reg-ver">${esc(p.version)}</span></h3>
        <p>${p.description ? esc(p.description) : '<span class="reg-dim">No description.</span>'}</p>
      </a>
    </li>`;
}

// The search runs in the page over the list it already holds, by the rule `kama pkg search` applies to the same
// catalog (docs/packages.md § Searching a registry), so the two always agree. Without JavaScript the list is all.
const SEARCH = `<script>
(() => {
  const form = document.querySelector('.reg-search'), input = form.querySelector('input');
  const list = document.querySelector('.reg-list'), none = document.querySelector('.reg-none');
  const all = [...list.children];
  const lower = s => s.replace(/[A-Z]/g, c => c.toLowerCase());
  const score = (li, words) => {
    const name = lower(li.dataset.name), bare = name.includes('/') ? name.slice(name.indexOf('/') + 1) : name;
    const desc = lower(li.dataset.description), kws = lower(li.dataset.keywords).split(' ').filter(Boolean);
    let total = 0;
    for (const w of words) {
      const s = name === w || bare === w ? 100 : name.includes(w) ? 20 : kws.includes(w) ? 10
              : kws.some(k => k.includes(w)) ? 5 : desc.includes(w) ? 1 : 0;
      if (!s) return 0;
      total += s;
    }
    return total;
  };
  const run = () => {
    const words = lower(input.value).split(/\\s+/).filter(Boolean);
    let shown = all;
    if (words.length) {
      shown = all.map(li => [score(li, words), li]).filter(([s]) => s > 0)
                 .sort((a, b) => b[0] - a[0] || (a[1].dataset.name < b[1].dataset.name ? -1 : 1)).map(([, li]) => li);
    }
    list.replaceChildren(...shown);
    none.hidden = shown.length > 0;
    const url = new URL(location.href);
    if (words.length) url.searchParams.set('q', input.value.trim()); else url.searchParams.delete('q');
    history.replaceState(null, '', url);
  };
  input.value = new URLSearchParams(location.search).get('q') || '';
  input.addEventListener('input', run);
  form.addEventListener('submit', e => { e.preventDefault(); run(); });
  if (input.value) run();
})();
</script>`;

function indexPage() {
  const n = packages.length;
  return shell({
    url: '/',
    title: 'Packages — kama',
    description: `${n} kama package${n === 1 ? '' : 's'}: search them, and add one with kama pkg add.`,
    site,
    content: `<main class="page reg">
  <header class="reg-head">
    <h1>Packages</h1>
    <p class="lede">${n} package${n === 1 ? '' : 's'}, each version immutable and pinned by its sha256. Add one with
    <code>${esc(installLine('<name>', '0.0.0').replace('^0.0.0', '<range>'))}</code>, or search from a terminal with
    <code>kama pkg search &lt;words&gt;</code>.</p>
    <form class="reg-search" role="search" action="/" method="get">
      <input type="search" name="q" placeholder="Search by name, description or keyword" aria-label="Search packages" autocomplete="off">
    </form>
  </header>
  <ul class="reg-list">
${packages.map(listItem).join('\n')}
  </ul>
  <p class="reg-none" hidden>No package matches every word.</p>
  <p class="note">This host is also what the toolchain reads: <code>/&lt;name&gt;/index.json</code> for a package's
  versions and <code>/catalog.json</code> for search — the protocol is in
  <a href="https://kama-lang.org/docs/packages/#the-registry-is-a-static-file-tree">docs/packages</a>.</p>
</main>`,
    scripts: SEARCH,
  });
}

function revisionCell(v, repository) {
  const sha = /^git:([0-9a-f]{7,40})$/.exec(v.revision || '')?.[1];
  if (!sha) return esc(v.revision || '');
  const forge = /^https:\/\/(github\.com|gitlab\.com|codeberg\.org)\/[^/]+\/[^/]+/.exec(repository || '')?.[0];
  return forge ? `<a href="${escAttr(`${forge.replace(/\.git$/, '')}/commit/${sha}`)}" rel="noopener"><code>${sha.slice(0, 12)}</code></a>`
               : `<code title="${escAttr(sha)}">${sha.slice(0, 12)}</code>`;
}

function packagePage(p, readme) {
  const latest = p.versions[0];
  const deps = Object.entries(latest.dependencies || {});
  const meta = [
    p.license ? `<dt>License</dt><dd>${esc(p.license)}</dd>` : '',
    p.repository ? `<dt>Repository</dt><dd><a href="${escAttr(p.repository)}" rel="noopener nofollow">${esc(p.repository.replace(/^https:\/\//, ''))}</a></dd>` : '',
    p.keywords?.length ? `<dt>Keywords</dt><dd class="reg-kw">${keywordLinks(p.keywords)}</dd>` : '',
    `<dt>Dependencies</dt><dd>${deps.length ? deps.map(([d, spec]) =>
        (byName.has(d) ? `<a href="${pageUrl(d)}">${esc(d)}</a>` : esc(d)) + (spec.version ? ` <code>${esc(spec.version)}</code>` : '')).join(', ') : 'none'}</dd>`,
  ].join('\n      ');
  const rows = p.versions.map(v => {
    const date = publishedOn(p.name, v.version);
    const tarball = new URL(v.tarball, origin + '/').href;
    return `      <tr><td><code>${esc(v.version)}</code></td><td>${date ? `<time datetime="${date}">${date}</time>` : ''}</td>` +
           `<td>${revisionCell(v, p.repository)}</td><td><a href="${escAttr(tarball)}"><code title="${escAttr(v.integrity)}">${esc(v.integrity.slice(0, 19))}…</code></a></td></tr>`;
  }).join('\n');
  return shell({
    url: pageUrl(p.name),
    title: `${p.name} — kama packages`,
    description: p.description || `${p.name}, a kama package.`,
    site,
    content: `<main class="page reg">
  <p class="reg-crumb"><a href="/">Packages</a></p>
  <h1>${esc(p.name)} <span class="reg-ver">${esc(latest.version)}</span></h1>
  ${p.description ? `<p class="lede">${esc(p.description)}</p>` : ''}
  <div class="reg-meta">
    <section>
      <h2>Install</h2>
      <pre class="code" data-lang="sh"><code>${highlight(installLine(p.name, latest.version), 'sh', words)}</code></pre>
      <p class="note">or in <code>kama.json</code>: <code>"${esc(p.name)}": { "version": "^${esc(latest.version)}" }</code></p>
    </section>
    <dl>
      ${meta}
    </dl>
  </div>
  <section>
    <h2>Versions</h2>
    <table class="reg-versions">
      <thead><tr><th>Version</th><th>Published</th><th>Revision</th><th>Integrity</th></tr></thead>
      <tbody>
${rows}
      </tbody>
    </table>
  </section>
  <article class="prose reg-readme">
${readme ? renderReadme(readme) : '<p class="reg-dim">This version has no README.</p>'}
  </article>
</main>`,
  });
}

function sitemap() {
  const urls = ['/', ...packages.map(p => pageUrl(p.name))];
  return `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n` +
         urls.map(u => `  <url><loc>${esc(origin + u)}</loc></url>`).join('\n') + `\n</urlset>\n`;
}

// ---------------------------------------------------------------- write

rmSync(outDir, { recursive: true, force: true });
cpSync(regDir, outDir, { recursive: true, filter: src => !path.basename(src).startsWith('.tmp-') });
cpSync(path.join(root, 'site/styles.css'), path.join(outDir, 'styles.css'));
for (const asset of ['fonts', 'favicon-16.png', 'favicon-32.png', 'apple-touch-icon.png', 'logo-68.webp', 'logo-68.png'])
  cpSync(path.join(root, 'site/assets', asset), path.join(outDir, 'assets', asset), { recursive: true });

writeFileSync(path.join(outDir, 'index.html'), indexPage());
for (const p of packages) {
  const file = path.join(outDir, p.name, 'index.html');
  mkdirSync(path.dirname(file), { recursive: true });
  writeFileSync(file, packagePage(p, await readmeOf(p)));
}
writeFileSync(path.join(outDir, 'sitemap.xml'), sitemap());
console.log(`registry site: ${packages.length} package page${packages.length === 1 ? '' : 's'} → ${outDir}`);
