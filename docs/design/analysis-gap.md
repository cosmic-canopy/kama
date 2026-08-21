# The analysis gap — the test-infra holes

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** This file used to carry the whole strict-numeric campaign, milestones 0 through 6.
**All of it has shipped**, at `0.9.28`, and the record lives in [SPEC.md](../SPEC.md) (*Arithmetic on two
values of one type* and *There is no implicit numeric conversion*), [docs/agents.md](../agents.md) and the
git log. One row still points at this file:

- **ROADMAP row 12 — test-infra holes** (§2 below). Unchanged and still open; it moved into the 1.0 gate
  2026-08-19, so it is no longer a NEXT-track item.

§1 — the uninstantiated-generic gap — **SHIPPED** `0.9.38`–`0.9.43`, and its design doc is deleted with
this note in its place. A generic nobody instantiates is now fully analyzed — function, type and `enum`
alike — because each type parameter stands for an opaque type promising exactly what its bounds promise.
The record is in [SPEC.md](../SPEC.md) (*Generics*: what is checked at a declaration versus at its
instantiation) and in `tests/xfail/generic_*`; the reasoning is in the git log.

## 2. Test-infra holes

- `tests/trap/` is **skipped** under `KAMA_SAN`, `KAMA_WASM`, and on Windows/MSYS2 (`run_tests.sh`, the
  `TRAP_OK`/`WASM`/`SAN_FLAGS` gate) — UBSan intercepts the trap and node's abort codes differ.
- **No leg runs MSan.** MSan-origins catches what ASan and wasm both miss.
- **`fs_raii` hangs on the wasm leg intermittently, and a HUNG kill cannot say why.** Seen once in a full
  `./dev matrix` (2026-08-19); the harness captured `state: S (sleeping)`, `wchan: futex_do_wait`,
  `syscall: 98` (futex), `open fds: 18` — so the fd RAII the fixture exists to test was working, and the
  process was blocked on a lock, not leaking or spinning on I/O.

  **Pre-existing, and established as unrelated to whatever is in flight** rather than assumed: the fixture
  ran 40/40 clean under node in isolation, its emitted C never references the shims that changed, and that
  C was byte-identical before and after. It needs the LOADED leg — ~1160 fixtures fanned across every core
  plus the leg's background node servers — which is exactly the shape that makes it rare and useless to
  bisect.

  **The watchdog is now instrumented for the next occurrence**, which is the only useful response to a
  hang that will not reproduce on demand. On the kill it captures, before any signal:

  - **every thread's** state/wchan/syscall, not just the main one — the question a `futex_do_wait` raises
    is *a lock is held, by whom*, and under emscripten NODEFS node runs a libuv threadpool, so what
    matters is whether the workers are idle (main waiting on work that never arrives) or parked too;
  - **`eu-stack -p`** (elfutils, added to the image) — the frames. It resolves real symbols on a parked
    node: `FutexEmulation::WaitJs32`, `uv_run`, `uv_cond_wait`, per thread;
  - on the macOS leg, **`sample`**, trimmed to the call graph — which names the kama source line.

  No capability change was needed: checked first, and the container already ptraces a sibling
  (`ptrace_scope` is 0, it runs as uid 0). elfutils rather than gdb — a few MB against a hundred-plus —
  and as the LAST image layer, so the Chromium/Playwright layers stay cached.

  All of it rehearsed end-to-end through `run_tests.sh` against a deliberately hanging fixture, on both
  legs, rather than reasoned about. That rehearsal found three things wrong with the instrumentation
  itself: `timeout` does not exist on macOS (so the wrapper silently swallowed the whole `sample` call),
  `sample`'s `Binary Images:` dump pushed the frames out of the line budget, and `wait "${arr[@]}"` on an
  empty array aborts the runner under `set -u`. `KAMA_TESTS_DIR` now points the harness at a chosen
  fixture set: `./dev fixture <name>` already ran one fixture, but on the host only and without the
  watchdog, so neither the wasm leg nor the hang instrumentation could be exercised on a single fixture.

  Node's own report still cannot cover this shape, and its ABSENCE remains the finding: an idle-but-alive
  event loop writes one, and a main thread parked in a syscall never reaches the handler. Until the hang
  is actually diagnosed this is a known watch item, not a green-suite guarantee — the subject of this
  section.
- **An `xfail` fixture never links, so it never reaches ASan.** This is why a campaign about *rejection*
  cannot take a green suite as its acceptance test: the view-window campaign's findings ④⑤⑥ survived all
  three legs and 34 guards. The discipline that works is a probe ledger walking every branch of the rule
  including the ones that must stay SILENT — milestone 6 used a 48-branch one and it caught two bugs a
  green matrix did not (a `case true:` fixture that was a parse error reading as a pass, and a literal
  exemption that rejected `int8 a = 2 + 3`).

## What shipped, so it is not re-derived

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
