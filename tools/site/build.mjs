// Assemble the deployable static site into _site/ (the Cloudflare Pages direct-upload input).
//
//   node tools/site/build.mjs      (normally reached via `tools/build-site` / `./dev site`)
//
// Everything in site/ ships verbatim. On top of that this writes the home page and one page per
// entry in pages.mjs, rendered from the repo's own markdown so the site has no second copy of any
// doc. The installers and llms.txt are copied to the root so kama-lang.org/install.sh serves the
// same file the installer and `kama update` reference.

import { readFileSync, writeFileSync, mkdirSync, rmSync, cpSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { PAGES, GROUPS, ORIGIN } from './pages.mjs';
import { renderDoc, checkAnchors } from './render.mjs';
import { highlight, kamaWords } from './highlight.mjs';
import { shell, sidebar, toc, template, fill, setVersion } from './layout.mjs';

const root = fileURLToPath(new URL('../..', import.meta.url));
const out = path.join(root, '_site');
const words = kamaWords(root);
const version = readFileSync(path.join(root, 'VERSION'), 'utf8').trim();
setVersion(version);

const write = (url, html) => {
  const file = path.join(out, url.replace(/^\//, ''), 'index.html');
  mkdirSync(path.dirname(file), { recursive: true });
  writeFileSync(file, html);
};

// ---------------------------------------------------------------- static files

rmSync(out, { recursive: true, force: true });
cpSync(path.join(root, 'site'), out, { recursive: true });
for (const f of ['install.sh', 'install.ps1', 'llms.txt']) {
  cpSync(path.join(root, f), path.join(out, f));
}

// ---------------------------------------------------------------- home page

// The fixture opens with a note explaining why it lives in tests/; that is for readers of the
// repo, not visitors, so the leading comment block is dropped before the code goes on the page.
const sample = readFileSync(path.join(root, 'tests/site_sample.kama'), 'utf8')
  .replace(/^(\/\/[^\n]*\n)+/, '').trimEnd();
writeFileSync(path.join(out, 'index.html'), shell({
  url: '/',
  title: 'kama — a no-GC language that compiles to C',
  description: 'kama is a C-family language with deterministic RAII instead of a garbage collector, ' +
               'named parameters, and no runtime. It compiles to portable C11 — native, WebAssembly, or bare metal.',
  bodyClass: 'home',
  content: fill(template('home.html'), {
    scene: template('scene.html'),
    sample: highlight(sample, 'kama', words),
    version,
  }),
  scripts: '<script src="/app.js" defer></script>',
}));

// ---------------------------------------------------------------- docs pages

const rendered = [];
for (const page of PAGES) {
  const src = path.join(root, page.src);
  if (!existsSync(src)) throw new Error(`build-site: ${page.src} is in the manifest but not in the repo`);
  const doc = renderDoc(readFileSync(src, 'utf8'), page.src, { root, words, page });
  rendered.push({ page, doc });

  write(page.url, shell({
    url: page.url,
    title: `${doc.title} — kama`,
    description: page.blurb || doc.intro.slice(0, 180),
    bodyClass: 'doc',
    content: `<div class="doc-wrap">
${sidebar(page.url)}
<article class="prose">
${doc.html}
<p class="edit"><a href="https://github.com/cosmic-canopy/kama/blob/main/${page.src}" rel="noopener">Edit this page on GitHub</a></p>
</article>
${toc(doc.toc)}
</div>`,
  }));
}

checkAnchors(rendered.map(({ page, doc }) => ({ url: page.url, src: page.src, slugs: doc.slugs, links: doc.links })));

// ---------------------------------------------------------------- docs index, 404, sitemap

const cards = GROUPS.map(g => {
  const items = rendered.filter(r => r.page.group === g).map(({ page, doc }) => `
    <a class="card" href="${page.url}">
      <h3>${page.nav}</h3>
      <p>${page.blurb || doc.intro.slice(0, 160)}</p>
    </a>`).join('');
  return `  <h2>${g}</h2>\n  <div class="cards">${items}\n  </div>`;
}).join('\n');

write('/docs/', shell({
  url: '/docs/',
  title: 'Documentation — kama',
  description: 'Guides and reference for the kama language: getting started, the language tour, the specification, packages, editors and targets.',
  bodyClass: 'doc-index',
  content: `<main class="page">
  <h1>Documentation</h1>
  <p class="lede">Everything below is generated from the repository's own markdown, so the site and the
  source can never disagree.</p>
${cards}
</main>`,
}));

writeFileSync(path.join(out, '404.html'), shell({
  url: '/404.html',
  title: 'Not found — kama',
  description: 'That page does not exist.',
  bodyClass: 'doc-index',
  content: `<main class="page">
  <h1>404</h1>
  <p class="lede">That page does not exist. Try the <a href="/docs/">documentation index</a>
  or head <a href="/">home</a>.</p>
</main>`,
}));

const urls = ['/', '/docs/', ...PAGES.map(p => p.url)];
writeFileSync(path.join(out, 'sitemap.xml'),
  `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n` +
  urls.map(u => `  <url><loc>${ORIGIN}${u}</loc></url>`).join('\n') +
  `\n</urlset>\n`);

console.log(`built _site/ — ${urls.length} pages, v${version}`);
