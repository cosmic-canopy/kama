# The analysis gap — the test-infra holes

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** This file used to carry the whole strict-numeric campaign, milestones 0 through 6.
**All of it has shipped**, at `0.9.28`, and the record lives in [SPEC.md](../SPEC.md) (*Arithmetic on two
values of one type* and *There is no implicit numeric conversion*), [docs/agents.md](../agents.md) and the
git log. One row still points at this file:

- **The test-infra-holes row** (§2 below) — still open, and in the 1.0 gate since 2026-08-19.
  ⚠️ Cited by TEXT, not by number: rows are renumbered whenever one is deleted, and this pointer named a
  "row 12" that has been three different items since.

§1 — the uninstantiated-generic gap — **SHIPPED** `0.9.38`–`0.9.43`, and its design doc is deleted with
this note in its place. A generic nobody instantiates is now fully analyzed — function, type and `enum`
alike — because each type parameter stands for an opaque type promising exactly what its bounds promise.
The record is in [SPEC.md](../SPEC.md) (*Generics*: what is checked at a declaration versus at its
instantiation) and in `tests/xfail/generic_*`; the reasoning is in the git log.

## 2. Test-infra holes

- `tests/trap/` is **skipped** under `KAMA_SAN`, `KAMA_WASM`, and on Windows/MSYS2 (`run_tests.sh`, the
  `TRAP_OK`/`WASM`/`SAN_FLAGS` gate) — UBSan intercepts the trap and node's abort codes differ.
- **No leg runs MSan.** MSan-origins catches what ASan and wasm both miss.
- **The compiler is never sanitized — SHIPPED** `0.9.117` (2026-08-29), see *What shipped* below.
- A campaign about *rejection* cannot take a green suite as its acceptance test: the view-window
  campaign's findings ④⑤⑥ survived all three legs and 34 guards. The discipline that works is a probe
  ledger walking every branch of the rule including the ones that must stay SILENT — milestone 6 used a
  48-branch one and it caught two bugs a green matrix did not (a `case true:` fixture that was a parse
  error reading as a pass, and a literal exemption that rejected `int8 a = 2 + 3`).

## What shipped, so it is not re-derived

- **A sanitized compiler — SHIPPED** `0.9.117` (2026-08-29). `make KAMA_ASAN=1 out/<platform>-asan/kama`
  builds the compiler itself with ASan, and `tools/check-compiler-asan.sh` drives the whole corpus through
  it in three passes: `check --each` over all 1,252 single-file fixtures (every rejection path),
  `transpile` over the 670 positives (the emit walk, which `check` never enters — it runs
  `CEmitter::analyze()` with no output stream), and `tools/check-lsp.sh` re-run with `$KAMA` pointed at the
  sanitized binary (`kama.lsp.cpp` + `kama.query.cpp` — the only long-lived kama process, so the one where
  a use-after-free compounds instead of being reclaimed at exit). ~74 s on the host.
  **The whole corpus came back clean.**
  - ⚠️ **The compiler does not leak, and the "leak-by-design" reading of it was wrong.** Counting
    `free`/`delete` sites (13 across 34k lines) measures nothing here: the AST is `std::shared_ptr` end to
    end (`SharedAST`/`SharedStatement`/… in `kama.forward.h`) with no parent back-pointers, so RAII frees
    it and no shared_ptr cycle is structurally possible. **Measured** over 80 fixtures on Linux with LSan
    verified armed (a probe leaking 1234 bytes got reported): **zero leaks**. So `detect_leaks=1` is a real
    assertion, not noise to suppress, and it is on wherever the platform has LSan. macOS has none, so the
    guard drops to the use-after-free/overflow assertion there — which is why this is NOT container-only,
    the way this file previously assumed.
  - ⚠️ **`exitcode=86` alone does not detect an ASan report.** Darwin's ASan defaults to
    `abort_on_error=1` and aborts (rc 134) before consulting `exitcode`, so the guard tests rc==86, rc>=128
    **and** greps for the report text. Keying on any single one of those misses a platform.
  - The batch is a pre-filter only, as in `run_tests.sh`'s analysis leg: ASan aborts the process, so a bad
    chunk loses its 32 verdicts and its files are re-run solo to name the one fixture.
  - ⚠️ **The guard was verified by making it FAIL** — a temporary heap-use-after-free in `CEmitter::analyze`
    gated on one fixture's name, confirmed to be caught, isolated to `tests/xfail/move_reuse_same_scope.kama`
    and reported with a symbolized trace, then reverted. A guard written against already-fixed code is the
    repeat failure in this repo's history; do not believe a green one that has never been seen red.

- **The `fs_raii` wasm hang — DIAGNOSED AND CLOSED** `0.9.83` (2026-08-25). It was a known **node**
  shutdown bug ([nodejs#54918](https://github.com/nodejs/node/issues/54918)) — not kama, not wasm, not
  V8 — and `run_tests.sh` now runs node with `--no-concurrent-recompilation` (`:309`, which says so).
  ⚠️ kama does **not** depend on node: `kama run` is native-only and the wasm target's real host is a
  browser, which cannot hit this. The only cost was our own gate's flakiness, so do not over-invest again.
  This section carried it as an open watch item with a page of watchdog instrumentation for four days
  after it was answered; pruned 2026-08-29.

Kept only as a pointer, because each cost a cycle to learn and the code no longer shows why:

- **`exprClass` returns `""` for a primitive AND for "unknown"**, and ~40 callers depend on `""` meaning
  "not a class, take the raw-C path". Every classifier built for this campaign (`typeOfExpr`,
  `callReturnTypeRaw`, `indexElemTypeRaw`, `moduleStaticCTypeRaw`) exists because of that conflation.
  **If a rule is silent on something, suspect this filter before suspecting the rule** — it hid a
  primitive three separate times.
- **A green `./dev matrix` proves little for a REJECTION rule.** Write the `xfail` first, and check the
  fixture matches the rule's own WORDING — an error is not evidence that *your* error fired.
- **`cType` is not injective** (`char` → `uint32_t`, `usize` → `size_t`), so `char + uint32` is
  indistinguishable from `uint32 + uint32` to a rule keyed on the lowered type. Pinned as permissive in
  `tests/op_exempt.kama` rather than left to be rediscovered. Separating them needs `primKey`, which
  wants a type NODE that `typeOfExpr` deliberately does not produce.
- **`--strict-numeric`** — the hidden measuring instrument (a TSV on stdout, no `usage()`, no docs) is
  still in the tree. It sized every decision in milestone 6 and is the tool for sizing the next one; its
  lesson is that **a measurement hiding its blind spot is worse than no measurement** (`op-unknown` and
  `unknown-src` are reported for that reason, and lumping comparisons into `arith-subint` once
  overstated a change by 300x).
