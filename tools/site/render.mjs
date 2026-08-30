// Markdown → HTML for the docs pages.
//
// Three things here are load-bearing and each one fails the build loudly rather than shipping
// something subtly wrong:
//
//   1. Slugs match GitHub's algorithm exactly, so the anchors already written into the repo's
//      docs (e.g. `packages.md#local-overrides--kamalocaljson`) keep working on the site.
//   2. Every relative link is resolved against the repo. A doc that is on the site links to the
//      site; anything else links to GitHub; a link to a path that does not exist throws. That is
//      the link checker — free, at build time, with no extra tooling.
//   3. Raw HTML in a doc throws. The docs contain none today; this keeps it that way, so a stray
//      `<T>` outside a code span can never be silently swallowed by the browser.

import { Marked } from 'marked';
import { statSync, existsSync } from 'node:fs';
import path from 'node:path';
import { REPO, BRANCH, bySrc, HOSTED_FILES } from './pages.mjs';
import { highlight } from './highlight.mjs';

/** GitHub's heading-slug algorithm: lowercase, drop punctuation (and ✅/🚧), spaces → dashes. */
export function slugify(text) {
  return text
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')          // a link in a heading contributes its text
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\p{M}\p{Pc} -]/gu, '')      // strips punctuation, backticks and emoji
    .replace(/ /g, '-');
}

function rewriteLink(href, srcFile, root) {
  if (/^(https?:|mailto:|data:)/i.test(href)) return { href, external: true };
  if (href.startsWith('#')) return { href, external: false };

  const [rawPath, frag] = href.split('#');
  if (!rawPath) return { href, external: false };

  const rel = path.normalize(path.join(path.dirname(srcFile), rawPath)).replace(/\\/g, '/');
  const abs = path.join(root, rel);
  const hash = frag ? '#' + frag : '';

  const page = bySrc.get(rel);
  if (page) return { href: page.url + hash, external: false };
  if (HOSTED_FILES.has(rel)) return { href: '/' + rel, external: false };

  if (!existsSync(abs)) {
    throw new Error(`${srcFile}: link to "${href}" resolves to "${rel}", which does not exist in the repo`);
  }
  const kind = statSync(abs).isDirectory() ? 'tree' : 'blob';
  return { href: `${REPO}/${kind}/${BRANCH}/${rel}${kind === 'blob' ? hash : ''}`, external: true };
}

/**
 * Render one markdown file.
 * @returns {{title: string, html: string, toc: Array<{depth:number,slug:string,text:string}>, intro: string}}
 */
export function renderDoc(md, srcFile, { root, words, page = {} }) {
  const toc = [];
  const slugs = new Set();
  const links = [];
  const seen = new Map();
  let title = null;
  let intro = '';

  const marked = new Marked({
    gfm: true,
    renderer: {
      heading(token) {
        const text = this.parser.parseInline(token.tokens);
        const base = slugify(token.text);
        const n = seen.get(base) ?? 0;
        seen.set(base, n + 1);
        const slug = n ? `${base}-${n}` : base;
        slugs.add(slug);

        if (token.depth === 1 && title === null) {
          title = token.text.replace(/`/g, '');
          return `<h1 id="${slug}">${text}</h1>\n`;
        }
        if (token.depth <= 3) toc.push({ depth: token.depth, slug, text: token.text.replace(/`/g, '') });
        return `<h${token.depth} id="${slug}">${text}` +
               `<a class="anchor" href="#${slug}" aria-label="Permalink to this section">#</a>` +
               `</h${token.depth}>\n`;
      },

      link(token) {
        const { href, external } = rewriteLink(token.href, srcFile, root);
        if (!external) {
          const [target, frag] = href.split('#');
          if (target && !/(\/|\.\w+)$/.test(target)) {
            throw new Error(`${srcFile}: internal link "${href}" must end in a slash`);
          }
          links.push({ target: target || page.url, frag });
        }
        const attrs = external ? ' rel="noopener"' : '';
        const title = token.title ? ` title="${token.title}"` : '';
        return `<a href="${href}"${title}${attrs}>${this.parser.parseInline(token.tokens)}</a>`;
      },

      code(token) {
        const lang = (token.lang || '').split(/\s+/)[0];
        return `<pre class="code"${lang ? ` data-lang="${lang}"` : ''}><code>` +
               highlight(token.text, lang, words) + `</code></pre>\n`;
      },

      html(token) {
        throw new Error(`${srcFile}: raw HTML is not allowed in docs — found ${JSON.stringify(token.raw.slice(0, 60))}`);
      },
    },
  });

  // Strip the claim→fixture markers BEFORE parsing. `tools/check-doc-claims.sh` requires a
  // `<!-- xfail: name -->` (or `<!-- test: name -->`) beside every negative claim in the docs, tying
  // the claim to the fixture that proves it — 151 of them across SPEC, KEYWORDS, TYPE_MODEL and the
  // tour. They are internal cross-references, never page content, so the site drops them.
  //
  // ⚠️ Done HERE and not in the `html` renderer below, for two reasons the obvious fix gets wrong:
  //   - a marker at the START of a line makes marked tokenize the WHOLE PARAGRAPH as block HTML
  //     (SPEC.md:3504 is one), so the token also holds real prose — returning '' would delete it;
  //   - one marker sits INSIDE a code fence (SPEC.md:3036), where it would otherwise render as
  //     literal noise in a displayed code sample.
  // Matching check-doc-claims.sh's own pattern rather than stripping comments generally keeps the
  // raw-HTML ban below at full strength: any OTHER HTML comment still reaches marked and still throws.
  // The lookbehind takes the space BEFORE a marker only when real text precedes it, so
  // `…is enforced <!-- xfail: n -->.` closes up to `…is enforced.` instead of leaving a space before
  // the period. A marker that starts its line keeps that line's indentation, which several inside
  // list items depend on.
  md = md.replace(/(?<=\S)[ \t]*<!--\s*(?:xfail|test):[^>]*-->|<!--\s*(?:xfail|test):[^>]*-->/g, '');

  const html = marked.parse(md);

  // Every doc opens with a single H1 and an abstract paragraph; that paragraph is the blurb
  // fallback for the docs index and the page description.
  const firstPara = html.match(/<p>([\s\S]*?)<\/p>/);
  if (firstPara) intro = firstPara[1].replace(/<[^>]+>/g, '').replace(/\s+/g, ' ').trim();

  return { title: title ?? path.basename(srcFile), html, toc, intro, slugs, links };
}

/**
 * Cross-page check: every internal link with a #fragment must point at a heading that exists.
 * Rewriting alone cannot catch this — a renamed heading leaves a link that resolves to a page but
 * lands nowhere. Run once, after every page has been rendered.
 */
export function checkAnchors(pages) {
  const slugsByUrl = new Map(pages.map(p => [p.url, p.slugs]));
  const bad = [];
  for (const { url, src, links } of pages) {
    for (const { target, frag } of links) {
      if (!frag) continue;
      const known = slugsByUrl.get(target);
      if (!known) continue;                       // a link to a non-doc page (e.g. /docs/)
      if (!known.has(frag)) bad.push(`${src}: "${target}#${frag}" — no such heading (from ${url})`);
    }
  }
  if (bad.length) throw new Error(`build-site: dead anchors\n  ` + bad.join('\n  '));
}
