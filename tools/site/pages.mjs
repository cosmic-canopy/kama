// The site manifest — the one place that knows which repo docs become pages, and where.
//
// Every entry is a real markdown file in the repo: the website never keeps its own copy of a
// doc. Adding a page here is the whole job of publishing a doc; `render.mjs` uses this same
// table to rewrite cross-document links (a link to a doc that is on the site points at the
// site; a link to anything else points at GitHub).

export const REPO = 'https://github.com/cosmic-canopy/kama';
export const BRANCH = 'main';
export const ORIGIN = 'https://kama-lang.org';

// Sidebar order is group order, then order within this list.
export const GROUPS = ['Start', 'Reference', 'Guides'];

export const PAGES = [
  { src: 'docs/GETTING_STARTED.md',    url: '/docs/getting-started/', nav: 'Getting started',  group: 'Start',
    blurb: 'Install the toolchain, write a first program, build it for native or the browser, and set up your editor.' },
  { src: 'docs/tour.md',               url: '/docs/tour/',            nav: 'Language tour',    group: 'Start',
    blurb: 'The whole language in one read — ownership, named parameters, match, generics, concurrency.' },

  { src: 'docs/SPEC.md',               url: '/docs/spec/',            nav: 'Specification',    group: 'Reference',
    blurb: 'The complete language reference: every type, statement, keyword and standard-library module.' },
  { src: 'docs/TYPE_MODEL.md',         url: '/docs/type-model/',      nav: 'Type model',       group: 'Reference',
    blurb: 'Why a type is a value, a resource, a view or a contract — and what each choice buys you.' },
  { src: 'docs/KEYWORDS.md',           url: '/docs/keywords/',        nav: 'Keywords',         group: 'Reference',
    blurb: 'Every reserved word, what it does, and the attributes the compiler recognises.' },
  { src: 'docs/FLOOR.md',              url: '/docs/floor/',           nav: 'Prelude floor',    group: 'Reference',
    blurb: 'What is in scope with no import at all — the surface that survives --no-std and bare metal.' },

  { src: 'docs/packages.md',           url: '/docs/packages/',        nav: 'Packages',         group: 'Guides',
    blurb: 'Projects, dependencies, the lockfile, publishing, and managing toolchain versions.' },
  { src: 'docs/editors.md',            url: '/docs/editors/',         nav: 'Editor setup',     group: 'Guides',
    blurb: 'One language server, eight editors: VS Code, Neovim, Vim, Emacs, Sublime, Helix, Kate, Zed.' },
  { src: 'docs/agents.md',             url: '/docs/agents/',          nav: 'AI agents',        group: 'Guides',
    blurb: 'Ask the compiler instead of guessing: kama query, the AGENTS.md kama ships, and why `check` is not the type check.' },
  { src: 'docs/targets.md',            url: '/docs/targets/',         nav: 'Targets',          group: 'Guides',
    blurb: 'Native, WebAssembly and cross-compilation — target triples and how to get a toolchain.' },
  { src: 'docs/mcu.md',                url: '/docs/mcu/',             nav: 'Microcontrollers', group: 'Guides',
    blurb: 'Bare-metal kama: a Cortex-M firmware image you can run under QEMU or flash to a board.' },
  { src: 'docs/benchmarks/RESULTS.md', url: '/docs/benchmarks/',      nav: 'Benchmarks',       group: 'Guides',
    blurb: 'Measured against C, C++, Rust, Go, C#, Java, Node, Lua and Python — with the methodology.' },
];

export const bySrc = new Map(PAGES.map(p => [p.src, p]));

// Root files the site hosts itself, so a doc linking to them stays on the site.
export const HOSTED_FILES = new Set(['install.sh', 'install.ps1', 'llms.txt']);
