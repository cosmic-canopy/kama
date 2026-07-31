# Fonts

Self-hosted so the site makes no third-party request (nothing render-blocking off-origin, no
visitor IP handed to a font CDN) and so `./dev serve` renders correctly offline.

| File | Family | Weights | Source |
| --- | --- | --- | --- |
| `jetbrains-mono.woff2` | JetBrains Mono | 400–600 (variable) | Google Fonts, v24, latin subset |
| `shippori-mincho-b1-600.woff2` | Shippori Mincho B1 | 600 | Google Fonts, v14, latin subset |
| `shippori-mincho-b1-800.woff2` | Shippori Mincho B1 | 800 | Google Fonts, v14, latin subset |

Both families are licensed **SIL Open Font License 1.1**, which permits redistribution.

These are the **latin subsets only** (`unicode-range: U+0000-00FF …`). The site's handful of
decorative kanji (鎌 一 二 三 無 盾 名 型 果 並 算 橋 具) deliberately fall through to a system
mincho — see `--font-jp` in `site/styles.css`. Shipping Japanese coverage would cost megabytes per
weight for a dozen glyphs.

To refresh: request the CSS from `fonts.googleapis.com/css2?family=…` with a modern browser
user-agent, take the `src:` URL from each `@font-face` whose `unicode-range` contains
`U+0000-00FF`, and download it here.
