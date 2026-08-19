# The analysis gap — the test-infra holes

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** This file used to carry the whole strict-numeric campaign, milestones 0 through 6.
**All of it has shipped**, at `0.9.28`, and the record lives in [SPEC.md](../SPEC.md) (*Arithmetic on two
values of one type* and *There is no implicit numeric conversion*), [docs/agents.md](../agents.md) and the
git log. One row still points at this file:

- **ROADMAP row 14 — test-infra holes** (§2 below). Unchanged and still open; it moved into the 1.0 gate
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

  **The watchdog now also samples EVERY thread**, not just the main one, which is the question a
  `futex_do_wait` actually raises: a lock is held, by whom? Under emscripten NODEFS node runs a libuv
  threadpool, so the shape that matters is whether the workers are idle (the main thread is waiting on
  work that never arrives) or themselves parked (a real cycle). Verified against a live threaded node
  process rather than assumed — the capture separates `ep_poll` threads from `futex_do_wait` ones.

  ⚠️ **Still missing, and it needs an image change: a native backtrace.** The container has no `eu-stack`,
  `gdb`, `lldb` or `pstack`, and `/proc/<pid>/stack` is privileged — so naming the actual frame would take
  elfutils in the image plus `CAP_SYS_PTRACE`. Worth doing only if the per-thread picture turns out not to
  be enough. Note that node's own report cannot cover this: its ABSENCE is itself the finding, because an
  idle-but-alive event loop writes one and a main thread parked in a syscall never reaches the handler.
  Until it is diagnosed, this is a known watch item, not a green-suite guarantee — which is the whole
  subject of this section.
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
