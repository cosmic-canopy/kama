// Build-time syntax highlighting. No JS ships to the browser: fenced code blocks and the home
// page's sample come out of here as <span>-tagged HTML.
//
// The kama keyword lists are READ FROM editor/vscode/syntaxes/kama.tmLanguage.json rather than
// retyped here, because that grammar is already guarded by tools/check-syntax.sh and
// tools/check-syntax-drift.sh — one list, one CI guard. If its shape ever changes, the extractor
// throws at build time instead of quietly colouring nothing.

import { readFileSync } from 'node:fs';

const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const tag = (cls, text) => `<span class="${cls}">${esc(text)}</span>`;

// ---------------------------------------------------------------- keyword extraction

function everyPattern(grammar) {
  const out = [];
  (function walk(node) {
    if (Array.isArray(node)) return node.forEach(walk);
    if (node && typeof node === 'object') {
      if (typeof node.match === 'string') out.push({ name: node.name, match: node.match, node });
      Object.values(node).forEach(walk);
    }
  })(grammar);
  return out;
}

/** The `\b(a|b|c)\b` alternation carried by a named scope. */
function scopeAlternation(pats, scope) {
  const m = pats.filter(p => p.name === scope).map(p => p.match.match(/\\b\(([^)]+)\)\\b/)).find(Boolean);
  if (!m) throw new Error(`highlight: kama.tmLanguage.json has no \\b(...)\\b pattern for scope "${scope}"`);
  return m[1].split('|');
}

/**
 * The alternation containing `word`, from wherever it appears. The type-kind words
 * (`value`/`resource`/…) and `enum` live in multi-capture declaration patterns rather than a
 * scope of their own, because in the real grammar they are contextual identifiers.
 */
function alternationWith(pats, word) {
  const re = new RegExp(String.raw`\((?!\?)([^()]*\b${word}\b[^()]*)\)`);
  const m = pats.map(p => p.match.match(re)).find(Boolean);
  if (!m) throw new Error(`highlight: kama.tmLanguage.json has no alternation containing "${word}"`);
  return m[1].split('|');
}

export function kamaWords(root) {
  const grammar = JSON.parse(readFileSync(`${root}/editor/vscode/syntaxes/kama.tmLanguage.json`, 'utf8'));
  const pats = everyPattern(grammar);
  return {
    keyword: new Set([
      ...scopeAlternation(pats, 'keyword.control.kama'),
      ...scopeAlternation(pats, 'keyword.other.modifier.kama'),
      ...scopeAlternation(pats, 'keyword.other.kama'),
      ...scopeAlternation(pats, 'keyword.operator.new.kama'),
      ...alternationWith(pats, 'enum'),
    ]),
    // `value`/`resource`/`view`/`contract` are CONTEXTUAL: a kind only directly after `type`, and
    // ordinary identifiers everywhere else (`int32 value = 5;`, `Ok(value: x)`). Colouring them
    // unconditionally turned every `value:` argument label into a keyword.
    kinds: new Set(alternationWith(pats, 'resource')),
    modifiers: new Set(scopeAlternation(pats, 'keyword.other.modifier.kama')),
    primitive: new Set(scopeAlternation(pats, 'storage.type.primitive.kama')),
    literal: new Set([...scopeAlternation(pats, 'constant.language.kama'),
                      ...scopeAlternation(pats, 'variable.language.kama')]),
  };
}

// ---------------------------------------------------------------- the tokenizers

// One ordered alternation; first match wins. Everything not matched is escaped as-is.
const KAMA_RE = new RegExp([
  /\/\/[^\n]*/,                                     // line comment
  /\/\*[\s\S]*?\*\//,                               // block comment
  /@"(?:[^"\\]|\\[\s\S])*"/,                        // verbatim string
  /"(?:[^"\\\n]|\\[\s\S])*"/,                       // string
  /'(?:[^'\\\n]|\\[\s\S])*'/,                       // char
  /@[A-Za-z_][A-Za-z0-9_]*/,                        // attribute
  /\b\d[\w.]*\b/,                                   // number (with kama's 42i32 / 1.5f32 suffixes)
  /\b[A-Za-z_][A-Za-z0-9_]*\b(?=\s*:(?!:))/,        // named-argument label — the signature of the language
  /\b[A-Za-z_][A-Za-z0-9_]*\b/,                     // word: keyword / type / call / plain
].map(r => r.source).join('|'), 'g');

/**
 * A `"…"` string with its `${…}` holes coloured separately — the editor grammar does this
 * (meta.interpolation.kama), so the site should too, or interpolation reads as inert text. A verbatim
 * `@"…"` never interpolates and is passed through whole. `\${` is an escaped literal, not a hole.
 */
function kamaString(text) {
  if (text.startsWith('@')) return tag('t-str', text);
  let out = '', last = 0;
  for (const m of text.matchAll(/\$\{([^}]*)\}/g)) {
    if (text[m.index - 1] === '\\') continue;             // escaped: `\${` is a literal
    out += tag('t-str', text.slice(last, m.index));
    last = m.index + m[0].length;
    // Inside a hole: the value path, then an optional `:spec` (`${pi:.2}`, `${n:0x}`).
    const body = m[1].replace(/^([^:]*)(:.*)?$/, (_, path, spec) =>
      tag('t-var', path) + (spec ? tag('t-fs', spec) : ''));
    out += tag('t-ip', '${') + body + tag('t-ip', '}');
  }
  return out + tag('t-str', text.slice(last));
}

function kama(src, words) {
  let out = '', last = 0;
  let afterType = false;   // inside `type [modifiers] <kind>` — the only place a kind word is a keyword
  for (const m of src.matchAll(KAMA_RE)) {
    const t = m[0];
    out += esc(src.slice(last, m.index));
    last = m.index + t.length;
    const after = src.slice(last);
    const isWord = /^[A-Za-z_]/.test(t);
    const kindHere = afterType && words.kinds.has(t);
    if (isWord && !t.startsWith('@')) {
      if (t === 'type') afterType = true;
      else if (!words.modifiers.has(t)) afterType = false;
    }
    if (t.startsWith('//') || t.startsWith('/*')) out += tag('t-cm', t);
    else if (t.startsWith('@') && !/^@"/.test(t)) out += tag('t-at', t);
    else if (/^["'@]/.test(t)) out += kamaString(t);
    else if (/^\d/.test(t)) out += tag('t-num', t);
    else if (kindHere) out += tag('t-kw', t);
    else if (words.keyword.has(t)) out += tag('t-kw', t);
    else if (words.primitive.has(t)) out += tag('t-ty', t);
    else if (words.literal.has(t)) out += tag('t-lit', t);
    else if (/^[a-z_]/.test(t) && /^\s*:(?!:)/.test(after)) out += tag('t-arg', t);
    else if (/^[A-Z]/.test(t)) out += tag('t-ty', t);
    else if (/^\s*\(/.test(after)) out += tag('t-fn', t);
    else out += esc(t);
  }
  return out + esc(src.slice(last));
}

const SHELL_RE = /#[^\n]*|"(?:[^"\\]|\\[\s\S])*"|'[^']*'|\$\w+|(?:^|\n)\s*\$?\s*[\w./-]+/g;
function shell(src) {
  let out = '', last = 0;
  for (const m of src.matchAll(SHELL_RE)) {
    const t = m[0];
    out += esc(src.slice(last, m.index));
    last = m.index + t.length;
    if (t.startsWith('#')) out += tag('t-cm', t);
    else if (/^["']/.test(t)) out += tag('t-str', t);
    else if (t.startsWith('$') && /\w/.test(t[1] ?? '')) out += tag('t-num', t);
    else out += t.replace(/([\w./-]+)$/, (_, w) => tag('t-fn', w));
  }
  return out + esc(src.slice(last));
}

const JSON_RE = /"(?:[^"\\]|\\[\s\S])*"(\s*:)?|\/\/[^\n]*|\b(?:true|false|null)\b|-?\b\d[\d.eE+-]*/g;
function json(src) {
  let out = '', last = 0;
  for (const m of src.matchAll(JSON_RE)) {
    const t = m[0];
    out += esc(src.slice(last, m.index));
    last = m.index + t.length;
    if (t.startsWith('//')) out += tag('t-cm', t);
    else if (m[1]) out += tag('t-arg', t.slice(0, -m[1].length)) + m[1];
    else if (t.startsWith('"')) out += tag('t-str', t);
    else if (/^[-\d]/.test(t)) out += tag('t-num', t);
    else out += tag('t-lit', t);
  }
  return out + esc(src.slice(last));
}

/** Highlight `src` as `lang`. Unknown languages are escaped, never mangled. */
export function highlight(src, lang, words) {
  switch ((lang || '').toLowerCase()) {
    case 'kama': return kama(src, words);
    case 'sh': case 'bash': case 'shell': case 'console': case 'powershell': return shell(src);
    case 'json': case 'jsonc': return json(src);
    default: return esc(src);
  }
}
