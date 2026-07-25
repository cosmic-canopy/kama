# MCU step 6 — inline assembly, intrinsics, const-eval (design of record)

**Status: 6a SHIPPED (2026-07-24, `4811d23`).** Inline assembly (the language-surface headline of MCU
step 6) is built: `asm("…")` inside `unsafe { }` → `__asm__ __volatile__("…" : : : "memory")`; fixtures
`tests/asm_nop.kama` + `tests/support/embedded_asm.kama` (transpile-grep in `tools/check-embedded.sh`).
`alignof(T)` was already shipped (fixture added). This remains the design-of-record brief; the sections
below describe as-built for 6a and design-of-record for the deferred 6b const-eval extensions. Steps 1–5
shipped 2026-07-23 ([MCU_READINESS.md](../MCU_READINESS.md), [ROADMAP.md](../ROADMAP.md) §5). Step 6 splits
into **language work** (this doc) and **build/library work** (toolchain triples, linker scripts, vendor
HALs, soft-float, AVR — *not* language, tracked in MCU_READINESS Tier 2 + ROADMAP §5).

## Scope — what is actually language work

| Item | Reality (verified 2026-07-23) | Effort |
|---|---|---|
| **Inline assembly** (`asm("...")`) | ❌ genuinely missing — no `asm` in grammar/emitter. **The headline of step 6.** | S–M |
| **`alignof(T)`** | ✅ **ALREADY SHIPPED** — fully wired (lexer `alignof`→`ALIGNOF`, grammar reuses `SizeofNode.isAlign`, emitter `_Alignof(cType)`, accepted in `isConstInitExpr`). Verified end-to-end (`alignof(Blk)`=8, `alignof(int32)`=4). **MCU_READINESS listed it ❌ by mistake.** Needs only a **fixture + doc fix**, ~5 min. | trivial |
| **Const-eval extensions** | 🟡 partial — literals / `sizeof` / `alignof` / const arithmetic on *literals* fold; **arithmetic on const-generic params (`N+1`) and named-const refs (`BASE`) do NOT**. MCU-motivated (baud divisors, lookup-table sizes). Follow-on to asm. | M |
| Soft-float, toolchain packaging, AVR | build/library, not language — out of scope here (see MCU_READINESS Tier 2). | — |

**Recommended order:** 6a inline asm (headline) → the trivial alignof fixture+doc → 6b const-eval
extensions (only if a concrete need lands; YAGNI). Naked functions (`@naked`) and extended-asm operands are
deferred (below).

---

## 6a — Inline assembly (the design)

### Why it's language, not library
`wfi`/`wfe` (idle-sleep), `cpsid i`/`cpsie i` (interrupt-masked critical sections), `dsb`/`dmb`/`isb`
(barriers), and cycle-exact delays have **no C-level equivalent** — they need `__asm__ __volatile__(...)`,
which the emitter must produce. A *curated set of named helpers* (`wfi()`, `disable_interrupts()`,
`barrier()`) is then an ordinary **library** (`std::mcu`, a thin follow-on) built on the `asm(...)`
primitive — the GOALS "unsafe core, safe/ergonomic API as library" model, exactly like the collections over
`unsafe`/`Ptr`. **Step 6a builds the primitive; the intrinsic library is a separate, later, non-language
task.**

### Surface (decisions)
- **Syntax:** `asm("wfi");` — a **statement**, paren form (familiar from C/Rust; `asm` reads as an
  operation). Reserve `asm` as a keyword (like `try` in step 5). One string-literal argument; **no string
  interpolation** (`InterpolatedStringNode` rejected — asm text must be literal).
- **Must be inside `unsafe { }`.** Inline asm is the ultimate raw operation → it belongs in the existing
  narrow, greppable `unsafe` seam (GOALS §5 "`unsafe { }` for raw memory"). Enforced in the emitter via the
  existing `_inUnsafe` flag (same gate as raw-pointer index/store). Outside `unsafe` → a clear diagnostic.
- **Always `__volatile__`.** MCU asm must never be optimized away or reordered out. Emit
  `__asm__ __volatile__("...")` unconditionally — there is no non-volatile form (one way, safe default).
- **Always a memory clobber (v1 decision).** Emit `__asm__ __volatile__("..." : : : "memory")` — every
  `asm(...)` is *also* a full compiler memory barrier. This makes `cpsid i`/`dsb`/`dmb` **correct by
  default** (their entire purpose is ordering; without the clobber the compiler may hoist memory ops across
  them — a silent footgun). A `nop` delay with a memory clobber is harmless. Trade: a hair of pessimization
  around each asm, which is the right default for correctness. A no-clobber form can be added later *if* a
  measured need appears (YAGNI). **Document this in KEYWORDS/SPEC so users know asm is a compiler barrier.**
- **Not gated to `--target embedded`.** Allowed anywhere inside `unsafe`, like `extern "<header>"` — the
  content is target-specific and the user owns portability (asm breaks "runs anywhere C runs", same caveat
  as FFI). Keep it simple; no target check.
- **Multiple instructions:** one string with `\n`-separated instructions (`"cpsid i\n\tdsb"`). The emitter
  must C-escape the `StringNode` value when writing the C string literal (newlines→`\n`, quotes, backslash)
  — reuse the existing string-literal C-escape helper (the one behind `kama_string_lit`/StringNode
  emission); do **not** splice the raw decoded bytes in (the recon draft did — that's a bug for multi-line).

### Seam map (verified 2026-07-23; line numbers drift — re-grep)
- **Lexer** [kama.l] keyword table (~line 334, alphabetical near `alignof`): add `{"asm", ASM}`.
- **Grammar** [kama.y]: declare `%token <string> ASM` (line ~144, beside `SIZEOF ALIGNOF TRY`). Add an
  `asm_statement` alternative to `embedded_statement` (the list at ~kama.y:758-770, beside
  `unsafe_statement`/`spawn_statement`):
  ```
  asm_statement
    : ASM LPAREN STRING_LITERAL RPAREN SEMICOLON
      { $$ = std::make_shared<AsmNode>(SCANNER_CODEGENCONTEXT, $3); }
    ;
  ```
  ⚠️ **bison `_opt` trap** (recorded in every prior step): if any optional is factored out, every `_opt`
  alternative MUST set `$$`. The rule above adds no `_opt`, so it's safe.
- **AST** [kama.ast.h]: add `class AsmNode : public StatementNode { SharedString code; ... }` — sibling of
  `UnsafeNode` (kama.ast.h:330-335). `STRING_LITERAL` → `StringNode` (kama.ast.h:183-187); store its
  `value`.
- **Emitter** [kama.cemit.cpp]: dispatch in `emitStatement` (the dynamic_cast chain at ~1657-1708, after
  the `IsolateNode` branch), calling a new `emitAsm(AsmNode*, depth)`:
  ```cpp
  void CEmitter::emitAsm(AsmNode* a, int depth) {
      if (!_inUnsafe) { unsupported("inline `asm(...)` must be inside `unsafe { }`", a->line); return; }
      line(a->line); indent(depth);
      *_out << "__asm__ __volatile__(" << cStringLiteral(a->code) << " : : : \"memory\");\n";
  }
  ```
  where `cStringLiteral` is the existing C-string-escape helper. Declare `emitAsm` in [kama.cemit.h] beside
  the other `emit*` statement methods. `_inUnsafe` is [kama.cemit.h:611].
- **Editor** [editor/vscode/syntaxes/kama.tmLanguage.json]: add `asm` to the keyword highlight list — the
  **syntax-drift guard** (`tools/check-syntax-drift.sh`) FAILS the suite otherwise (step-5 lesson with
  `try`). Put it with `unsafe`/control keywords.

### Fixtures / tests
- `tests/asm_wfi.kama` — `unsafe { asm("nop"); }` in a fn; assert it builds + runs hosted (a `nop` is a
  legal x86 instruction, so it runs on the CI host — pick host-legal mnemonics for the runnable fixture;
  `wfi`/`cpsid i` are ARM-only so use them only in a **transpile-grep** check, not a run).
- **Transpile-grep** in `tools/check-embedded.sh` (step-6 section): assert `unsafe { asm("wfi"); }` lowers
  to `__asm__ __volatile__("wfi" : : : "memory")` in the emitted C (mirrors how `@interrupt` is
  transpile-grep-verified because the x86 host can't assemble ARM ISRs).
- `tests/xfail/asm_outside_unsafe.kama` — `asm("nop");` with no `unsafe` → rejected, `.msg` = `must be
  inside \`unsafe\``.
- Consider an `unsafe { asm("..."); }` inside the `embedded_blink` fixture path to prove it survives the
  `--target embedded` freestanding build.

### Deferred (call out, don't build in 6a)
- **Extended asm with operands** (`asm("mrs %0, primask", out: x)` — output/input/clobber constraint lists,
  GCC style). Needed to *read/write* a variable or special register (e.g. save/restore PRIMASK). Deferred:
  most MCU needs (`wfi`/barriers/`cpsid i`) are operand-less, and a save-restore critical section can use a
  small `extern` C shim in the interim. Design it as a second `AsmNode` form later.
- **`@naked` functions** (`__attribute__((naked))` — no prologue/epilogue, for a hand-written reset handler
  / context switch). Follows the `@interrupt`/`@section` `declAttrPrefix` pattern (kama.cemit.cpp:10178+):
  add a `naked` arm pushing `"naked"`. Cheap when needed; defer until a concrete case (YAGNI).
- **Top-level / module-scope asm** (`.section` directives). Rare; defer.
- **Stopgap available today:** `extern "<intrinsics.h>"` + `extern fn void wfi();` with a header
  `#define wfi() __asm__("wfi")` already works (MCU_READINESS:64) — so asm is *unblocked* even before 6a;
  6a makes it first-class and greppable.

---

## alignof (already shipped — just close it out)
Add `tests/alignof_basic.kama` (`alignof(int32)`==4, `alignof` of a `value` struct == its widest member,
usable in a `static`/const-init position) and correct the MCU_READINESS Tier-2 row + the `sizeof` doc to
say `alignof` ships alongside it. No compiler change. (This doc's companion commit already fixes the
readiness row.)

---

## 6b — Const-eval extensions (follow-on, only if needed)
Current ceiling ([kama.cemit.cpp] `isConstInitExpr` ~13894, `constValue`/`constArgN` ~3973/3992,
`_constSubst` [kama.cemit.h:514]): folds literals, `sizeof`/`alignof`, and binary/unary/cast/logical trees
**of literals**; binds a const-generic param to its literal (`Fixed<T,4>`→`N=4`). Gaps, MCU-motivated:
1. **Arithmetic on const-generic params** — `N+1`, `2*N` in a const context (e.g. a static array sized
   `N+1`). Extend `constValue()` to recursively fold `BinaryExpressionNode`/unary/cast when both operands
   resolve (literals or `_constSubst` bindings). This is the highest-value, most self-contained piece.
2. **Named-const references in const-init** — let `isConstInitExpr` accept an `IdentifierNode` that names a
   `const` local / module-static bound to a const value (resolve via a const-value table). Unblocks
   `static int32 buf[BUFSZ];`-style config.
3. **Const function calls (`constexpr`-style)** for build-time table generation (CRC/gamma/trig). High
   effort, lowest priority — only if a real workload demands it.

Scope 6b to (1) first; (2) and (3) are separate asks. None block asm.

---

## Doc/consistency checklist for the implementing session
- KEYWORDS.md: add `asm` (a keyword like `try`); note it requires `unsafe` and is a compiler memory barrier.
- SPEC.md: an "Inline assembly" subsection near the FFI/`unsafe` material.
- grammar.bnf: regenerate via `bash tools/gen-grammar` after the grammar change (it catches accumulated
  drift too — expected).
- MCU_READINESS.md: flip the "Inline assembly" row to ✅ on ship; the `alignof` row is corrected now.
- Run `tools/cdev test` (native) + `KAMA_SAN=1` + the embedded guard; `tools/check-syntax-drift.sh` must
  stay green (the `asm` highlight).
