// The page shell every page shares: <head>, the top nav, the footer, and the docs chrome
// (sidebar + table of contents). Templates that are mostly markup live in templates/*.html and
// are filled with a plain {{key}} substitution; anything structural is built here.

import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { GROUPS, PAGES, ORIGIN } from './pages.mjs';

const here = new URL('./templates/', import.meta.url);
export const template = name => readFileSync(new URL(name, here), 'utf8');

export const fill = (tpl, vars) =>
  tpl.replace(/\{\{(\w+)\}\}/g, (m, k) => (k in vars ? vars[k] : m));

export const escAttr = s => String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');

const BASE = template('base.html');

// A file under site/ that changes between deploys, addressed by its content: `/styles.css?v=<hash>`. Cloudflare
// serves CSS and JS with `max-age=14400`, so a phone that loaded the old stylesheet kept it for four hours after a
// deploy — a fixed nav still broken on the device it was fixed for. A changed file is a new URL no cache has
// seen; an unchanged one keeps its URL and stays cached. (Fonts and images are never edited in place.)
const versions = new Map();
export function assetUrl(name) {
  if (!versions.has(name)) {
    const bytes = readFileSync(new URL(`../../site/${name}`, import.meta.url));
    versions.set(name, createHash('sha256').update(bytes).digest('hex').slice(0, 10));
  }
  return `/${name}?v=${versions.get(name)}`;
}

// Which host a page is served from. kama-lang.org's pages link to each other by path; the package registry
// (registry.kama-lang.org, tools/site/registry.mjs) wears the same shell from another host, so its links to
// the docs carry kama-lang.org's origin, and its own section — Packages — is the one marked current.
export const PACKAGES = 'https://registry.kama-lang.org';
const MAIN_SITE = { origin: ORIGIN, home: '', section: '' };
export const REGISTRY_SITE = origin => ({ origin, home: ORIGIN, section: 'packages' });

export function shell({ url, title, description, bodyClass = '', content, scripts = '', jsonld = '', site = MAIN_SITE }) {
  return fill(BASE, {
    title: escAttr(title),
    description: escAttr(description),
    canonical: site.origin + url,
    css: assetUrl('styles.css'),
    bodyClass,
    nav: topNav(url, site),
    content,
    footer: footer(site),
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

function topNav(url, site) {
  const h = site.home;
  const on = p => (!site.section && url.startsWith('/docs/') && p === '/docs/' ? ' aria-current="page"' : '');
  const packages = site.section === 'packages' ? '/" aria-current="page' : PACKAGES + '/';
  return `<nav class="topnav">
  <a class="brand" href="${h}/">
    ${mark(34)}
    <span>kama</span>
  </a>
  <div class="topnav-links">
    <a href="${h}/docs/getting-started/">Get started</a>
    <a href="${h}/docs/tour/">Tour</a>
    <a href="${h}/docs/"${on('/docs/')}>Docs</a>
    <a href="${packages}">Packages</a>
    <a href="https://github.com/cosmic-canopy/kama" rel="noopener">GitHub</a>
  </div>
</nav>`;
}

let VERSION = '';
export const setVersion = v => { VERSION = v; };

function footer(site) {
  const h = site.home;
  return `<footer class="footer">
  <div class="footer-brand">
    ${mark(28)}
    <span>kama · v${VERSION} on the road to 1.0 · MIT OR Apache-2.0</span>
  </div>
  <div class="footer-links">
    <a href="${h}/docs/">Docs</a>
    <a href="${h}/docs/getting-started/">Getting started</a>
    <a href="${h}/docs/spec/">Spec</a>
    <a href="${PACKAGES}/">Packages</a>
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
