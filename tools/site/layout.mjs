// The page shell every page shares: <head>, the top nav, the footer, and the docs chrome
// (sidebar + table of contents). Templates that are mostly markup live in templates/*.html and
// are filled with a plain {{key}} substitution; anything structural is built here.

import { readFileSync } from 'node:fs';
import { GROUPS, PAGES, ORIGIN } from './pages.mjs';

const here = new URL('./templates/', import.meta.url);
export const template = name => readFileSync(new URL(name, here), 'utf8');

export const fill = (tpl, vars) =>
  tpl.replace(/\{\{(\w+)\}\}/g, (m, k) => (k in vars ? vars[k] : m));

export const escAttr = s => String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');

const BASE = template('base.html');

export function shell({ url, title, description, bodyClass = '', content, scripts = '', jsonld = '' }) {
  return fill(BASE, {
    title: escAttr(title),
    description: escAttr(description),
    canonical: ORIGIN + url,
    bodyClass,
    nav: topNav(url),
    content,
    footer: footer(),
    scripts,
    jsonld: jsonld ? `\n<script type="application/ld+json">${JSON.stringify(jsonld)}</script>` : '',
  });
}

// The brand mark: served at 68px (2x the largest on-page size), WebP with a PNG fallback. The
// 256px logo.png is NOT used on the page — it exists for og:image, where scrapers want a large PNG.
const mark = size => `<picture>
      <source srcset="/assets/logo-68.webp" type="image/webp">
      <img src="/assets/logo-68.png" alt="" width="${size}" height="${size}">
    </picture>`;

function topNav(url) {
  const on = p => (url.startsWith('/docs/') && p === '/docs/' ? ' aria-current="page"' : '');
  return `<nav class="topnav">
  <a class="brand" href="/">
    ${mark(34)}
    <span>kama</span>
  </a>
  <div class="topnav-links">
    <a href="/docs/getting-started/">Get started</a>
    <a href="/docs/tour/">Tour</a>
    <a href="/docs/"${on('/docs/')}>Docs</a>
    <a href="https://github.com/cosmic-canopy/kama" rel="noopener">GitHub</a>
  </div>
</nav>`;
}

let VERSION = '';
export const setVersion = v => { VERSION = v; };

function footer() {
  return `<footer class="footer">
  <div class="footer-brand">
    ${mark(28)}
    <span>kama · v${VERSION} on the road to 1.0 · MIT licensed</span>
  </div>
  <div class="footer-links">
    <a href="/docs/">Docs</a>
    <a href="/docs/getting-started/">Getting started</a>
    <a href="/docs/spec/">Spec</a>
    <a href="https://github.com/cosmic-canopy/kama" rel="noopener">GitHub</a>
  </div>
</footer>`;
}

/** The docs sidebar. A <details> on narrow screens, so the mobile menu needs no JavaScript. */
export function sidebar(currentUrl) {
  const groups = GROUPS.map(g => {
    const items = PAGES.filter(p => p.group === g).map(p =>
      `      <li><a href="${p.url}"${p.url === currentUrl ? ' aria-current="page"' : ''}>${p.nav}</a></li>`
    ).join('\n');
    return `    <p class="side-group">${g}</p>\n    <ul>\n${items}\n    </ul>`;
  }).join('\n');

  return `<details class="side" open>
  <summary>Documentation</summary>
  <nav>
${groups}
  </nav>
</details>`;
}

/** Per-page contents rail, from the H2/H3 headings collected while rendering. */
export function toc(entries) {
  if (entries.length < 3) return '';
  const items = entries.map(e =>
    `    <li class="d${e.depth}"><a href="#${e.slug}">${e.text}</a></li>`).join('\n');
  return `<aside class="toc"><p class="toc-title">On this page</p>\n  <ul>\n${items}\n  </ul>\n</aside>`;
}
