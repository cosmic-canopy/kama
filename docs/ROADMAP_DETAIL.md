# kama roadmap — detail

The reasoning behind every item in [ROADMAP.md](ROADMAP.md). That file is the **order**; this one is the
**why**, one section per topic. Nothing here is history — when an item ships, its record moves to the
permanent doc (see the table below) and its section is deleted from here.

> ### ⚠️ Maintaining these two files — read before editing either
>
> **[ROADMAP.md](ROADMAP.md) is the short ordered list and must stay short.** It carries one row per item:
> a one-line summary and a link into this file. No reasoning belongs there, ever — that is what sent the
> single-file version to 1,279 lines and made "what is next" unreadable. `tools/check-roadmap.sh` holds
> down the line ceiling, that every row resolves to a section here, and that no section here is orphaned.
>
> **When an item ships, DELETE it from both files.** A roadmap that also logs completions stops being
> readable as a plan, and the single-file version drifted that way twice.
>
> ⚠️ **Deleting a row does NOT renumber anything.** `KR-<n>` is a permanent id — assigned once, never
> reused, never renumbered — so a shipped row's id simply retires and leaves a gap, and every `KR-` written
> anywhere (here, in a commit message, in someone's notes) keeps meaning what it meant. A new row takes one
> more than the highest id present. This replaced position numbering, which silently re-pointed every
> `row N` in prose each time a row was deleted; `tools/check-roadmap.sh` now checks that ids are unique and
> that every `KR-` citation resolves, which is a check position numbering could not support.
>
> Before deleting, confirm the record lives where it belongs, and **migrate it there if it does not**:
>
> | What shipped | Where its record goes |
> | --- | --- |
> | Language surface (syntax, semantics, stdlib API) | [SPEC.md](SPEC.md) |
> | A campaign, while it is still in flight | its `docs/design/*.md` — **deleted when the work ships**, once the rows above carry its record |
> | A capability against a target domain | [MCU_READINESS.md](MCU_READINESS.md) · [ENGINE_READINESS.md](ENGINE_READINESS.md) · [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md) |
> | User-facing behavior + workflow | [packages.md](packages.md) · [editors.md](editors.md) · [mcu.md](mcu.md) · [targets.md](targets.md) |
> | *Why* a thing happened, and when | the git log — do not re-tell it here |
>
> What may stay behind is **at most a one-line pointer**, and only where a forward item depends on it.
> A **residual** of shipped work (a gap, a follow-on, a deferred optimization) stays — as its own forward
> item, stated as what is left to do, not as a recap of what was done.

<a id="s1"></a>

## 1. Remaining before 1.0

What the language *is* lives in [SPEC.md](SPEC.md); the engine/MCU capability matrices in
[ENGINE_READINESS.md](ENGINE_READINESS.md) / [MCU_READINESS.md](MCU_READINESS.md); the history in the git log.

**The language surface is feature-complete.** Anything that would *break* source has to land before the
tag or wait for 2.0.

**The docs/naming reconcile — CLOSED `0.9.98`, and the row was wrong about its own subject.** It was
scheduled as a NAMING pass (PascalCase types, lowerCamel methods, no `I`-prefix on contracts, lowercase
`string`). Measured across `lib/` and `prelude/`, every one of those conventions **already held** — no
`I`-prefixed contract, no non-PascalCase type declaration (the only lowercase ones are
`prelude/builtin.kama`'s primitives, which the convention wants lowercase), no non-lowerCamel public
method in `lib/std`. There was nothing to reconcile.

What was actually broken was **doc code that the compiler refuses**, and it was worse than stale: `SPEC.md`
says in prose *"Bare `int` is not a kama type"* and then used `int` as a type in seventeen of its own code
blocks. Those snippets were legal until the module campaign (`0.9.80`) made every C keyword a reserved
word; nothing noticed, because nothing reads the docs' code. Measured and fixed: **29 × bare `int`**
(SPEC.md), **10 × bare `float`** (TYPE_MODEL.md), and **8 ×** the `p::{X}` module-import spelling the same
campaign deleted (the form that parses is brace-first, `import { p::X };`) — all eight of those in prose
rather than in blocks. **13 fenced blocks were rejected by the lexer before; zero after.**

`tools/check-doc-spelling.sh` holds it down, deriving its flag set from `kama.l`'s own two tables (the C
words kama reserves, minus the ones kama uses) rather than listing them — hardcoding is how the docs
rotted in the first place. ⚠️ **It is a LEXICAL check on purpose.** Compiling every fenced block was the
first design and is wrong: 81 of 139 blocks do not parse standalone, and almost none of those is a defect
— they are deliberate fragments. That guard would mean annotating ~110 blocks with opt-out markers and
would not have caught one of the 47 real defects. A reserved word is refused wherever it appears, fragment
or not, which is why text is the right substrate here. The same extraction, aimed at negative claims
instead, is what the "~42 negative doc claims have no xfail link" row wants.

The **manifest** key set is reconciled: the entry field is `entry`, not `main` (npm's `main` names a
library's entry point for importers — the opposite end of the word), and `out` names the build-output
root. Both are read by `kama seed`, which is what would otherwise have propagated a wrong name into every
project created after it.

**A value-producing `match` is typed at CHECK time — shipped `0.9.37`.** ⚠️ **The row that tracked this was
right about the blind spot and wrong about what it cost, and the correction is the part worth keeping.**

The row said the cause was pass ordering: `emitMatchSwitch` binds an arm's payload during emission while
`rejectInitKindMismatch` runs earlier in `checkDeclaredTypes`. That path is **unreachable** — the only
shape `checkDeclaredTypes` could see is a `match` in a field initializer, and that is refused outright
(*"a value-producing `match` here needs a statement slot"*). Twenty probes across every ordinary position
— local init, assignment, return, free/method/collection argument, operand, ternary branch, nested match,
block arm, `ref` param, interpolation, `cast<T>(match …)`, `isize`/`usize` vs `int64`/`uint64`, and this
section's own `p.v` member-access example — were **already rejected**, because the destination is threaded
into `_matchTargetCType` and the rule fires on the ARM, one level down.

The real defect was **stale target inheritance**, and it was a silent miscompile:

```kama
fn int64 f(int8 p) { return cast<int64>(p); }
int64 y = f(p: true ? match (e) { case A(v: v): v; … } : match (e) { … });   // built clean, returned 44
```

Both gates that thread a destination (call argument, assignment) tested the node itself for being a
`MatchNode`, so the same match **wrapped in a ternary** slipped past — and a non-threading position left
`_matchTargetCType` alone rather than clearing it, so the match adopted the enclosing `int64`. The arm
check then passed, and an `int64` 300 reached an `int8` parameter as 44. The kind rule went the same way
(a number into a `bool p` was accepted). Two locks had to fail at once: the target was inherited, **and**
the argument-level check that should have caught it was silent because `typeOfExpr` answered `""` for a
`match` — which is the blind spot the row named.

Both are closed. `needsTargetType` looks through a ternary; non-threading positions now **clear** the
target so a miss fails closed on the existing "must appear in a typed position" error; `typeOfExpr` binds
each arm's payload types (via `matchSubjectClassQuiet` + `bindInstSubst` + `bindArmPayloadTypes`, shared
with the emitter) while classifying that arm. Two neighbours fell out and are fixed with it: the target
was also stale across an arm's **non-final statements**, and an **empty block arm** reached neither
arm-block check, emitting C that read the result temp uninitialized — caught only by clang, never by kama.

⚠️ Do not restore the **literal** fallback in `typeOfExpr`'s pass 2. A literal is contextually typed (D2a),
so it can only echo the destination (catching nothing) or contradict it (a false positive; `isize written =
match (wr) { case Ok(value: w): w; case Err(error: e): 0; }` was rejected as an `int32`). The fix is to
bind the payload types, not to guess — the `anyValueArm` gate stays exactly as it is.

**The classifier's blind spots, surveyed 2026-08-18** — measured with `kama check --strict-numeric` over
`tests/*.kama` **one file at a time** (checking them together makes one program and every `main` collides),
counting distinct `file:line` in the `unknown-src` + `op-unknown` buckets. The list in `typeOfExpr`'s
comment had drifted from the code in two places, both found by probe rather than by reading.

*Re-measured after the value-producing-`match` work (2026-08-18, `0.9.37`): **369 → 165** distinct blind
lines, and the `match`-shaped ones **220 → 16**. The 16 that remain were the generic-body family (closed `0.9.43`)
plus arms whose values are literal ternaries, not a residue of the match typing itself. **The corpus needed
no migration at all** — 1234 fixtures green with zero edits — which is itself the evidence that the arm
rule had been enforcing the same constraint all along, and therefore that this milestone's payoff was the
miscompile above and an honest instrument, never 220 silent numeric rules.*

| source | status | size |
|---|---|---|
| `foreach` binding | **closed** `0.9.31` | −69 lines |
| `match`-arm payload binding | **closed** `0.9.32` | −64 lines |
| `borrow` alias | **was already closed** — the comment outlived the code; the borrow site records both the C type and the type node | 0 |
| mixed arithmetic | **not a gap** — milestone 6 makes mixed operands an *error*, so there is no type to invent | 0 |
| value-producing `match` at check time | **closed** `0.9.37` | −204 lines |
| type parameter | **closed** `0.9.40`–`0.9.43` — an opaque type parameter gives `T` a type that promises what its bounds promise, so an expression typed by `T` resolves at the declaration | ~24 |
| comptime parameter | open — the residue of that work: a `comptime N: int32` stands for a VALUE, and a probe has none to invent without deciding the template's own `comptime assert`. Concentrated in `lib/std/num/fixed.kama` | ~19 |
| intrinsic / `extern fn` with no recorded return type | open, small — the `string.length()` class the isize campaign fixed one instance of | ~5 |
| a user **operator overload**'s result | open, small — **the table's missing row**, found 2026-08-18 by probe while writing the conformance fixtures: `(n + 3)` is `?` even though `operator+` declares `-> int32`. The operator-heavy files (`math/vec`, `quat`, `num/fixed`) are blind mostly for the *generic* reason above, so this is its own small bucket, not their cause | ~5 |

**What the binding milestone corrected about its own brief.** The migration was recorded here as 19
fixtures, measured by writing `_localCTypes[nm]` at the two `foreach` sites. The landed fix routes through
`_localTypeNodes` instead — read only by `neverNullType` and the numeric rules, where `_localCTypes` is
also read by `lvalueCType`, the compound-assignment path and the ownership/RAII paths — and breaks **six**.
`fs_roundtrip`, `net_tcp_options` and the `proc_*` family never fail through the classifier; those 13 were
the ownership paths reacting to a map they had no business seeing a binding in. The distinction matters
beyond the count: a `foreach (string s in …)` binding is a borrow on the indexed path and an owned value on
the iterator path, so teaching `_localCTypes` about bindings perturbs `ownsByValue` LHS detection.

**The gap showed in the emitted CODE**, now guarded by `tools/check-binding-widen.sh`: an unanswered source
falls back to `KAMA_NARROW`, so a *widening* `cast<int64>(v)` out of a binding carried a check that could
never fire. ⚠️ **It is not a release-speed claim, and this was measured rather than assumed** (2026-08-18):
at `-O2`/`-O3` clang inlines `kama_narrow_chk_s`, proves an `int32` always fits an `int64` and deletes it —
the same loop compiled with and without the check timed **identically** (0.11 s both, same clang, same
flags, a one-token diff in the C). The cost is a debug build and `--keep-c` readability, which is what the
README's "drops into an existing C codebase" rests on. **The "~19% of a 2M-iteration loop" figure recorded
during the cast trap does not reproduce for this shape** — do not repeat it without re-measuring what it
was actually about. Neither the exit code nor a timing can see this, which is why the guard reads the C.

**The Zed grammar pin follows the GRAMMAR — done, and no longer a scheduled row.** `editor/zed/extension.toml`
pins a *commit* and Zed fetches that rev, so the pin — not the working tree — is what Zed users get. It had
drifted 16 grammar changes and 19 days behind, which is why `slot` and named match patterns stopped
highlighting with nothing to say so.

This used to be scheduled as *"repoint it at the 1.0 tag"*, on the reasoning that a content guard could not
exist against a moving SHA — it would fail the very commit that changes the grammar. Two things were wrong
with that:

- **The trigger is the grammar, not the release.** Tying it to `VERSION` would fire on all 346 commits of
  that drift window when only 16 touched the grammar, and a pin that always moves signals nothing by moving.
- **The chicken-and-egg is one commit deep, not fatal.** The target is *computable* —
  `git log -1 --format=%H -- tree-sitter-kama` — and, decisively, **stable under its own fix**: the pin lives
  in `editor/zed/`, outside the grammar directory, so correcting it never moves the answer. So the guard
  converges in one step, and a grammar change simply lands as TWO commits (the change, then the pin), the
  same shape the `VERSION` rule already has.

Also wrong was a worry about pinning an unpushed commit: the pin travels in the same push as the grammar it
names, so no *published* state ever points at something the remote cannot serve.

`tools/check-editors.sh` §2d now asserts the pin IS the last grammar commit and prints the exact fix; §2c
still proves the rev resolves and carries a grammar at all. A tag pin was considered and rejected — a tag
only moves at releases, so it would be stale by this rule for the whole window between them.

Everything else here is library or toolchain work that does **not** gate the tag:

1. **`std::process` — async/Poller-driven *live* child-stream reads.** `run()` captures a finished child's
   output today; streaming a running child's stdout as it arrives is the piece left.
2. **Standard-library follow-ups — the M2 PARITY CAMPAIGN is COMPLETE** (M2a 2026-08-04; M2b
   `0.9.190`–`0.9.195` and M2c `0.9.196`–`0.9.197`, both 2026-09-05: sleep + wall clock, DNS, fs completion,
   `std::path`, stdio handles + `readLine`/`Lines`, then `std::random` and `std::encoding`). Its brief is
   deleted; the record is SPEC's per-module sections, and what stays here is the residue below.
   The bar is **Rust-`std` parity**: the only no-GC peer, and the only one whose stdlib also stops before
   regex/TLS/HTTP/crypto — which is the right line now that kama has a package manager.
   ⚠️ **So TLS, regex, HTTP and CIPHERS are DECLARED NON-GOALS for `std`, not unscheduled work**, and this
   is the sentence that says so. Refined 2026-09-06: **digests are in** (`std::digest::sha1`/`sha256`,
   SPEC § Digest) — a non-cryptographic protocol needs one (RFC 6455, git ids, content addressing, the
   registry's own integrity strings), and every batteries stdlib ships them; what stays out is the
   constant-time half — ciphers, key exchange, signatures — which is `@kama/sodium`'s job. Recorded emphatically because the first consumer's queue lists TLS with
   the status "ROADMAP" and is waiting for it: `wss://` is not coming to `std`, and the answer for a
   secure socket is a package or terminating TLS at a reverse proxy. A non-goal that reads like a
   backlog item gets re-triaged forever. No new language
   surface; pure library/codegen. The items below are what the campaign left open:
   - **`std::net`** — DNS **shipped `0.9.192`** as explicit `resolve`/`resolveOne` + `TcpStream.connectTo`
     (IPv4 only, because every socket seam is `AF_INET`; a name resolving to IPv6 alone reports
     `HostUnreachable` rather than an empty list). *(UDP and ephemeral-port `getsockname` ship —
     `lib/std/net/udp.kama`; IPv6 and multicast are separate, tracked in §2 — and DNS is the first
     concrete reason to want that row: the resolver already sees the AAAA records it has to drop.)*
**Post-1.0 — the decided big-arc sequence (with the user, 2026-07-26):**
1. **Editor tooling (§10).** The front end is a reusable query API with real source spans, which every
   later tool rides on; the residuals are in §10.
2. **Scripting / multimodal (§7) — the flagship 2.0.** The polymorphic-emitter → direct-wasm → bytecode-VM arc,
   driven by wanting a fast iteration/runtime tier for game engines + web. First concrete step: refactor the C
   emitter behind an abstract backend interface (C as the first impl), the shared lowering in the base.
3. **Self-hosting — the final-version capstone, LOWEST priority.** A maturity/dogfooding milestone, **not** an
   enabler: it rides on #1 (front-end-as-library) + #2 (runtime-into-kama), which is why §7 notes the
   runtime port makes multi-backend and self-hosting *the same project*. Do it last, when the language is stable.

**⚠️ Non-negotiable performance invariant (across all of the above).** kama is at **C parity today**, and the
**native/release tier (`kama → C → clang/emcc`) stays exactly as fast + lean — untouched.** Multimodal is
**strictly additive**: the same kama syntax *also* renders to direct-WASM and (later) a scripting VM as **separate
iteration tiers**, never a replacement for the C backend. Direct-WASM is "another hot-reload/scripting option" —
near-native and toolchain-free, but **not** as fast as native kama today (LLVM's optimizer + SIMD autovectorization
keep `C→emcc -O3` ahead on heavy numeric loops), so the **release tier remains the max-performance path for both
native and web**. Don't conflate "can emit WASM directly" with "the fast web path."

- **`std::time` calendar — scheduled, unsized.** `time.kama` says "deliberately no calendar … a half-calendar is
  worse than none", which means WHOLE, not never: a general-purpose stdlib needs civil dates. Scope: civil-from-days
  (Hinnant's algorithms), ISO-8601 format/parse of a `SystemTime`, leap-year and weekday arithmetic; no zone
  database (a package, as `chrono-tz` is), no locale.

**Fixture hygiene — the retired-syntax `tests/xfail` sweep, DONE 2026-09-08 (tests only).** The row said 23;
the class was **25**, measured on `0.9.253` by running all 758 xfails and reading their errors rather than
grepping for `public Name(`: 23 carried the class-named constructor (two of them without `public`, which the
grep missed) and two more failed only on the unimported `DynamicArray`. Each produced 2–13 errors where its
`.msg` asserts one, passing because the harness checks that the asserted message APPEARS — not a false green,
but a fixture erroring for four reasons no longer showed its program refused for the ONE it names, and the
extra errors could mask a position drift. All 25 now spell `public ctor make(…)` / `Type.make(…)` /
`new Type.make(…)` and import what they name; 22 produce exactly one error. The three that do not are the
compiler's own behaviour, not stale syntax, and are left as they are: `self_move_owned` and
`new_bare_stateful` report a second error from the SAME offending line (a cascade of the asserted one), and
**`iface_collection` reports twelve, every one inside the instantiated stdlib and none in its own file** — its
`DIAGNOSTIC_LINES` row is now `-`, which is the finding: a contract as a collection element is refused only
by cascade, so it is a ROADMAP row (S). The other 120 multi-error xfails are cascades of their own asserted
error (a refused `borrow` binder leaves its name unresolved, an abstract type with no `ctor` fails twice,
…), read and left alone. **No error-count guard** was added: `DIAGNOSTIC_LINES` already pins every position
a fixture's diagnostics name, so a spelling retired later would move a row and fail the suite — the count
guard would duplicate that and need a per-fixture allowlist for the cascades above.

<a id="s2"></a>

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The
language-completeness residual is **closed**; what remains here is genuinely later-track or opt-in.

### A declaration `@compileFor` drops is never checked (KR-54) — measured 2026-09-14, `0.9.340`

`pruneInactiveDecls` removes an inactive declaration from its unit before name pre-registration and
collection (SPEC *Conditional compilation*), so nothing downstream knows it existed — by design, and it is why
a gated-out declaration's symbols cannot leak. The cost is that NO rule reaches it. Measured on a loose file,
native host:

```kama
@compileFor(ARCH_WASM32)
fn int32 f(Zork z) { Optional<Zork> o = Optional::None; return 0; }
fn int32 main() { return 0; }
```

builds clean — the unknown `Zork` in the signature and the body are never resolved. The same holds for every
rule, not only names: a gated body is never type-checked, ownership-checked or emitted on this host. For a
language whose moat is portable C, that is the platform seam rotting on the targets a developer does not build
every day, and the first report arrives from the one who does.

Found while deciding where the name-resolution campaign's body checks live (`0.9.340`): a separate pre-emission walk was proposed partly to judge
code the compiler never emits, and measuring showed it would not have — pruning runs before either walk.

**Not the answer: judging names before pruning.** Names legitimately differ per target — an `extern fn`, an
`extern "<header.h>"` or a `type extern value` may exist on one target only, and a gated declaration may name
exactly those — so a pre-prune resolution pass would refuse correct code. **The likely answer** is running the
existing analysis once per declared target: a manifest names its targets, `kama check` walks each with that
target's flags active, and a diagnostic says which target it came from. Open questions for the design pass:
cost (N analyses — the pass is seconds, not minutes), how a loose build (no manifest, no declared targets)
answers, and whether `kama build` should warn when a manifest declares targets the build did not check.

### A generated host header, and `type expose value` (KR-52) — measured 2026-09-13

**Where it came from.** The by-value crossing rule (SPEC *FFI*, shipped `0.9.327`) was ruled as a PAIR — `type extern value` (a C header owns the layout) and
`type expose value` (kama owns and emits it) — and then measured across both function kinds before building:

| by value | `extern fn` (prototype from the C header) | `expose fn` (C calls kama) |
|---|---|---|
| plain `type value` | clang: *"incompatible type 'Pair'"* | builds; the host guesses a layout |
| `type extern value` | works (measured) | works (measured) |
| `type expose value` | **cannot work** — the header declares its own struct, so kama's is a second C type (mangled name) or a redefinition (bare name) | works; the host writes a matching typedef |

So `type extern value` covers every cell that can compile, and `expose` adds exactly one thing: kama as the
source of truth for a layout NO C header states, usable only by an `expose fn`. Without a generated header the
host has to hand-write that struct anyway, which is what `extern` already asks — so the rule shipped with one
marker (maintainer, 2026-09-13), and the two markers are not "extern and/or expose" on one type: each answers
WHO owns the layout, and only one side can.

**Wanted here:** `kama build` emits a host-includable header — every `expose fn` prototype and every by-value
type it names — and with it, decide `type expose value` (the header is what makes a kama-owned layout real for a
host). Measured facts to start from: struct names never reach the linker, so kama's scoped C name (`geo__Vec2`)
costs the ABI nothing and the header can spell the bare `Vec2`; an `expose fn` returning a callback is already
refused (`foreign_callback_expose_return`); ⛔ the maintainer requires `expose` for types to be revisited here.

**Ruled 2026-09-14 (maintainer), with the reasoning that decided each:**
- **`type expose value` is ADOPTED.** The marker answers who owns a C layout, exactly as `extern fn`/`expose fn`
  answer who owns a body: `type extern value` is a layout a C header states, `type expose value` one kama states
  and the generated header publishes. Binding a kama-owned layout through a hand-written header instead is two
  sources of truth, and drift between them is silent (measured: see the field check below). Unmarked value types
  never enter the header by layout — freezing a layout is an ABI promise and must be greppable (GOALS 5), and it
  keeps KR-22's elision free for every other type. A C-owned type may cross either function kind; a kama-owned
  one only an `expose fn` (an `extern fn`'s prototype is the header's). Its fields are C-representable and all
  visible to the host (no `private`), it is not generic, and it is built by ctors only.
- **`type extern value` fields are checked against the header** — SHIPPED `0.9.341`: size and arithmetic kind
  per field, a C11 `_Static_assert` (`tests/xfail/extern_value_layout.d`).
- **The header is always written** beside the output when a program has an `expose fn` — no flag, since a host
  that forgot one falls back to hand-written prototypes — and the project's own `csources` get it on their
  include path.
- **Exposed C names are QUALIFIED by module path**, joined with `_` — SHIPPED `0.9.342`. Bare names threw away the
  scoping modules exist for at exactly the boundary where a collision is hardest to see: two modules exposing
  `tick` were refused (and two of the three refusals were bugs), and an `expose fn open` broke any host
  translation unit that included `<fcntl.h>`. `@linkName` is the one override, on functions and on `type expose
  value`; two kama names meeting at one C name are refused. Deferred from this row: whether an enum crossing a
  boundary must spell its discriminants (implicit numbering renumbers silently when a variant is inserted).

### `drop` — SHIPPED `0.9.290`/`0.9.291`, kept here for the rule it established

`drop` takes an `UnsafePtr<T>` and destroys the pointee. The record, because the *rule* outlives the change:

**RAII covers a named local or by-value parameter in a lexical scope, and a bare block ends one** (measured).
`drop` exists for the places a scope structurally cannot reach — a heap pointee behind a raw pointer, and an
element in a hand-managed buffer — which is every legitimate use of it in the stdlib. It is one leg of the
manual-memory triad (`allocate` → place a value → **`drop`** → `deallocate`); without it you could free the
BYTES but never destroy what lived in them.

**Why the pointer form rather than "keep the place form, require `unsafe`":** the `unsafe` requirement comes
FREE (an `UnsafePtr` expression already requires an `unsafe fn`, so `drop` sits behind the same gate as its
two siblings), and a local is not a pointer — so the double drop is **unspellable** rather than diagnosed.

⚠️ **The evidence that the marker was the right gate, worth keeping because it generalizes:** of the 16
`drop()` sites in `lib/std` before the change, the **10 inside an `unsafe fn` were all correct**, and **6 of
the 7 in SAFE fns were the bugs** — a live crash in `SortedMap.put` replacing any owning key. Requiring the
marker then forced `DynamicArray.clear()` and `Deque.clear()` to be marked, which were the two remaining
legitimate sites hiding in safe functions, and surfaced two more latent double drops (`~Cell() { drop(value:
this.value); }` dropped a FIELD the owner's destructor already drops).

⚠️ **A field is auto-dropped by its owner's destructor** (measured) — so an explicit drop of `this.field` was
always a second one. ⚠️ **`addr(of: someLocal)` still launders a local into a pointer**, and that is accepted
on purpose: `addr(of: …)` requires an `unsafe fn`, so reaching it means the author took responsibility. What
the pointer form buys is that SAFE code cannot express the double drop at all.

**Parked, deliberately not folded in:** a safe type's containment of a raw pointer is invisible at its
declaration — `Owned`/`Shared`/`Weak` each hold private `UnsafePtr` fields (`Shared`/`Weak` two apiece:
pointee and control block). SPEC's decision table explicitly blesses the field (*"legal — every container and
every `type extern value` depends on it"*) and every member touching it is `unsafe`, so this is the sanctioned
pattern rather than a loophole. Making the containment visible would touch every container, not just the three
handles, so it is its own question.

### Three defects found while closing rows 9 and 10 (2026-09-11) — all PRE-EXISTING

Each was confirmed against a binary built before that session's first commit (`0.9.292+gfb4f9378`), so none
is a regression from the graph-delegate, element-store or `--no-heap` work.

**1. An early `return` from a `match` arm destructing the local that `match` is initializing — FIXED
`0.9.297`.** Kept for the rule: **a local is not live until its initializer completes**, and a
value-producing `match` is the one initializer that can leave without completing. The machinery already
existed — `slot` marks a hole `Moved` to mean "owns nothing right now" and scope cleanup skips a `Moved`
local — so the fix spells a mid-initializer local exactly that way. ⚠️ **Two things about how it hid**, both
general: every `deserialize` fixture in the corpus writes this shape and none ever FIRES the Err arm, since
a passing test deserializes something valid; and it can present as a clean WRONG EXIT CODE rather than a
crash when the stack happens to be zeroed, so **a green sanitizer leg was not evidence against it**.
`tests/match_init_early_return` exercises both arms and counts destructor runs through a static, which is
what makes it catch the defect (rc 241 against the pre-fix compiler) rather than merely survive it.

**2. ~~`friend` grants do not cross generics~~ — CLOSED `0.9.300`/`0.9.301`.** Kept for the rule and for
one correction. ⚠️ **The recorded diagnosis was wrong on a point, and the way it was wrong is the lesson:**
with BOTH sides generic the `unknown 'friend' accessor` refusal never appears at all. It cannot — a generic
owner's grant is never VISITED, because `resolveFriends` iterated `_classes` and a template lives in
`_genericTypes`, so the loud face and the silent face were never two bugs. That single omission also meant
the honesty checks (a typo'd member, a grant on an already-public one) never ran for a generic owner, which
was a third face nobody had noticed. **THE RULE: a grant crosses to the CORRESPONDING instance** —
`Tree<A,B>` reaches `Node<A,B>`, a sibling instance does not — because that is what plain `private` already
does between siblings (measured: `Node<int32>` reading `Node<int64>.secret` is refused), and a grant must
never be broader than the rule it relaxes. C++ agrees and makes it explicit (`friend struct Tree<K,V>;`
grants only the matching specialization; the bare spelling kama uses is a hard error there), so kama reads
its one spelling as the stricter meaning, leaving an explicit `friend Tree<K,A>[m];` additive if it is ever
wanted. ⚠️ **The `kama query` twin had drifted exactly as row 9's had** — a function or `Type::method`
accessor is recorded as a C name and the twin compared the KAMA name, so every non-class grant read as
invisible in completion while the compiler accepted it. Fixed together; the record is
[SPEC.md](SPEC.md#access-control-) and `tests/friend_generic`.

**3. ~~An `InlineArray` over a generic-instance element cannot share a program with `DynamicArray`~~ —
CLOSED `0.9.306`.** Kept for the rule: **a name is resolved where it was WRITTEN, never where a late
pass happens to be standing.** `registerFixed` stored the element unresolved, and `registerFixedViews` runs
once every unit is collected — under the context the LAST unit left, a stdlib module, where a user file's
private type resolves to nothing. ⚠️ **The row was narrower than the defect:** the filed shape
(`relocate(into:)` failing to infer) was the loud face, and probing found the plain one — `InlineArray<Point>`
for ANY user `Point` could not `viewMut()` at all, imports or no. `sorted_map.kama` escaped because its element
mentions the map's own parameters (no concrete instance at the first mint) and `BTreeFrame` lives in the one
namespace the late pass saw. The record is `tests/inline_array_user_elem_view`.

### Rows 9 and 10 CLOSED (`0.9.293`–`0.9.296`) — kept only for the rules they established

**`SortedMap` carries its graph (`0.9.293`).** ⚠️ **Row 9's recorded cause was WRONG, in this file and in
ROADMAP.md and in the campaign memory alike**, so the correction is the first thing worth keeping: it was
NOT that "graph discovery never reaches a nested helper". Discovery was fine — `seedField` already chained
`Owned<BTreeNode>` → type arguments → `Shared<V>`, so `V` was in the node set all along. The gap was that
the twin predicate keyed on the method **name** (`ci.methods.find("serialize")`), so `BTreeNode.serializeInto`
— a differently-named private helper, which is where a B-tree's element write actually lives — got no
graph-carrying copy and the object table died at the call.

**THE RULE: the graph's object table travels with the serializer, and nowhere else.** Three checks, and
the point of all three is that the closure cannot run away:

1. **Declaration gate** — a method carries the table only if it is the contract `serialize`/`deserialize`
   or is marked `@serializedGraphEdges` AND takes a `ref Serializer`/`Deserializer`. Taking the sink is
   already what makes a method a serialization method, so it is also what bounds this. ⚠️ **Measured blast
   radius:** all of `lib/std` outside the serde module has 8 `ref Serializer`-taking methods; 7 are the
   contract `serialize` and already carried the table, so the rule added exactly ONE.
2. **Call gate** — the table is threaded only when the call passes the enclosing copy's OWN sink. A body
   that builds a fresh `Serializer` is writing a different document, and stamping our ids onto it would be
   wrong. Enforced where the call is already emitted, so **no call-graph pre-pass exists**.
3. **Boundary refusal** — our sink handed to an unmarked, eligible method is a compile error naming both
   methods and the two fixes, because the failure it prevents is invisible at the site.

**One attribute, two roles**, told apart by the signature it sits on — no new language surface and no
grammar artifacts. A new attribute was considered and rejected: both roles answer one question ("the graph
routes through this member") and there is nothing for the author to choose between. See
[SPEC.md](SPEC.md) *Serialization* for the user-facing statement.

⚠️ **A delegate's body is emitted TWICE** — plain under its own C name (what a non-graph program calls) and
again under a `__graphTwin` suffix. `serialize` is the case that does NOT double up, because its plain name
is taken over by the synthesized root driver. The suffix is needed because the twin of a method named
`serializeInto` would otherwise BE that method's own C name.

**`SlotMap` (`0.9.296`) — the design question is answered: A HANDLE SURVIVES A ROUND TRIP.** A slot map
exists rather than an array plus indices precisely so a handle can outlive the value it names, so if a round
trip invalidated handles, serializing an entity or asset registry would be pointless. The compact
alternative was rejected for failing **silently**: a stale handle could coincidentally match a rebuilt slot
and alias the wrong value, which is the exact accident the generation check exists to prevent. The wire
preserves the slot layout; the format and its reasoning are in [SPEC.md](SPEC.md) with the code.

**Row 10 (`0.9.294`) — an assignment target and a `match` subject are PLACES.** The branch that claims a
statement on the RHS KIND spelled the LHS with `emitExpression`, which for an intrinsic-collection element
is the by-value `__get(…)`. ⚠️ **The row knew about ONE of four shapes**: a generic-instance ctor, a
qualified variant construction and an array literal all emitted unassignable C, while a `match` into a
PRIMITIVE element failed earlier and differently ("needs a typed assignment target") because `exprClass` of
an element access answers "" for a primitive element. ⚠️ **A second site was found by writing the fixture
rather than by reading the code**: a `match` borrows its subject by pointer and the emitter already
CLASSIFIED an element access as an lvalue, then took the address of the `__get(…)` rvalue — the same
classify-one-way/spell-the-other shape. ⚠️ **Blast radius was smaller than implied**: `InlineArray` is the
only type whose element store goes through `__set`; every kama-side container declares `ref T operator[]`
and took the place path all along.

⚠️ **`--no-heap` was relaxed to match its own sibling (`0.9.295`)**, because `Handle`'s hand-written serde
half exposed the difference: an attribute says "prove THIS body", but a **flag says "prove the PROGRAM",
and a program is what `main` reaches**. Rejecting an unprovable dispatch where it was EMITTED made the flag
mean "no body anywhere in the import closure may dispatch", so `import { std::collections::SlotMap };`
alone failed a `--no-heap` build. ⚠️ **Two sites were coupled**: the walk SKIPS a root holding its own
site, on the premise that the immediate gate reported it — so relaxing the gate alone left nobody
reporting, and the guard silently stopped firing until that skip was relaxed for indirect sites too.

- **`Fixed<B> comptime(int32 F)` does not implement `Real`.** A contract requires *every* method, so conformance
  means writing 21 fixed-point functions including `sin`/`cos`/`atan2`/`exp`/`log`/`cbrt` in Q-format —
  CORDIC and polynomial-approximation work, a numerical-methods project rather than a library chore. It is
  the obvious first customer of the exported `Real` contract (`lib/std/math/scalar.kama`).

- **Per-target primitive availability — considered, deliberately NOT built.** *If a target genuinely cannot
  supply a primitive, reject its uses with a kama diagnostic rather than a C-level assert.* The reasoning, so
  it is not re-derived: (1) it would not replace `kama_runtime.h`'s asserts, which answer "did the C compiler
  deliver my premise **on this build**, with this `--cc` and these flags" — the only mechanism that catches a
  wrong flag or a changed toolchain default, since kama accepts arbitrary triples and any `--cc`; a kama-side
  availability table is a second assumption about the toolchain, not a verification of it. (2) The motivating
  case is not real: AVR *has* `float64`, behind `-mdouble=64`. (3) The cost is a stdlib and source-language
  fork — `float64` appears in 8 `lib/std` files including `fmt/fmt.kama`, both serializers, `math/scalar`,
  `time` and `num/endian`, and a bare `1.0` **is** a `float64`, so banning the type makes `1.0` an error.
  (4) Rust, Zig and Go all guarantee a 64-bit IEEE float on every supported target; C guarantees `double`
  exists while permitting 32 bits — the exact hole the assert plugs. No mainstream language bans a float type
  per target. If a real case ever appears, the mechanism is the existing `@compileFor` decl-level prune plus a
  tag-type boundary, not a use-site predicate on primitives.

- **`fnptr` cannot take type parameters** — the only declaration form in kama that cannot
  (`type value X<T>`, `type contract C<T>`, `enum Result<T,E>` and `fn f<T>` all can). So a generic
  callback signature has no name: `fnptr Ordering Compare<T>(ref T a, ref T b);` does not parse
  ([kama.y](../src/kama.y), the `FNPTR` rule has no type-param slot). **Non-goal** (the consumer-driven audit, 2026-09-07; it used to read "deliberately deferred")
  — for the case it would serve, a generic **contract** is the better tool: it monomorphizes to a
  direct inlinable call where an `fnptr` is an indirect one, and a comparator object can carry state,
  which matters because kama has no capturing closures. `std::collections`' `Order<T>` is the worked
  example. A generic contract IS the generic callback, so nothing is missing. *(This used to defer itself "alongside
  the full specialization campaign", on the argument that a concrete-args specialization would cover the
  per-type-body case an `fnptr` gets reached for. That campaign is now a non-goal — see the entry below —
  and it changes nothing here: the generic-contract answer never depended on it.)*

- **Full generic specialization — a DECLARED NON-GOAL.** It was scheduled as campaign 2 of the contract
  model ("polymorphism for generic functions, across all types": a second body for a generic function,
  selected by fully-concrete type arguments, so any two are identical or disjoint). Scoping it against
  the tree killed it, on three counts:
  - **It re-opens the hole M6 closed, in a worse form.** Retro-impl (`implements C for T`) was deleted
    because it let a module reach into a type it does not own. A specialization lets a module reach into
    a *function* it does not own and change what that function does for a given type — program-wide,
    invisible at every call site, from any package that can see the generic. Retro-impl at least added a
    *named method* you could see on the type; a specialization silently replaces a body. Whole-program
    coherence catches a *duplicate* specialization and does nothing about a single one, which is the
    dangerous case.
  - **What is left over is contract design, which is the language's answer already.** `type contract`
    plus `type intrinsic` covers primitives, `string`, and every type you own — that is exactly what
    `std::math`'s `Real` is, and [scalar.kama](../lib/std/math/scalar.kama) says so in prose. The
    genuine remainder is a per-type body for a user type you do **not** own, and unlocking that is the
    thing we do not want.
  - **Nothing depends on it.** Comptime parameters' three blockers are all comptime-parameters-on-types issues;
    the view-escape check is independent; no site in `lib/`, `prelude/`, `tests/`, `examples/` or
    `bench/` needs it.

  Reopen only if a concrete case appears that a contract genuinely cannot express. Three real defects
  came out of scoping it and have shipped: a duplicate function declaration was silent (for a generic
  template the last body simply won), a function type parameter's `= Default` was parsed and dropped,
  and a type parameter shadowing a visible type said nothing. All three are pinned by `tests/xfail/`.

- **The safety/unsafe boundary — SWEPT, and the seam has since been MOVED (findings 1, 2, 9 closed).**
  The intended guarantee is that danger is isolated behind `unsafe`: nothing in safe kama should be able
  to produce UB, and a bug inside an `unsafe fn` is library-author territory. Every claim below was
  produced by compiling and running a probe under ASan/UBSan, never by reading — repros in the commit
  that lands this.

  **The root cause, which subsumed most of the individual holes — now CLOSED by the unsafe seam
  (findings 1, 2 and 9).** GOALS §3a said the safe surface never sees a raw pointer and that `unsafe { }`
  + `UnsafePtr<T>` guard the FFI boundary. In fact **only the dereference operator `p[i]` was gated**.
  Safe kama could freely *produce* a raw pointer (`addr(of:)`, a public `dataPtr()`, `cast<UnsafePtr<T>>`
  of an integer), *store* it in a field, and **call arbitrary C with it** — `extern fn` calls were not
  gated at all. Both programs below compiled; both are now rejected at compile time, by four independent
  rules in the first case. What the seam IS now lives in [SPEC.md](SPEC.md) and [GOALS.md](GOALS.md);
  what is left below is findings 3–8, 10 and 11.

  ```kama
  extern "<stdlib.h>";
  extern fn UnsafePtr malloc(usize n);
  extern fn void free(UnsafePtr p);
  fn int32 main() {
      UnsafePtr q = malloc(n: cast<usize>(64));
      free(p: q); free(p: q);        // ASan: attempting double-free. NO `unsafe` in this program.
      return 0;
  }
  ```
  and, with the only `unsafe` being an idiomatic "unsafe core, safe API" accessor a library author would
  reasonably write:
  ```kama
  type value Cell { UnsafePtr<int32> p;
      public ctor make(UnsafePtr<int32> p) { this.p = p; }
      public unsafe fn int32 get() { return this.p[0]; }
  }
  Cell c = Cell.make(p: addr(of: seed));
  { int32 tmp = 1234; c = Cell.make(p: addr(of: tmp)); }
  return c.get();                     // ASan: stack-use-after-scope
  ```
  No `View` is involved in the second — the whole `type view` escape apparatus that GOALS §3c says makes
  a borrow unable to dangle is simply bypassed by a plain `type value` with a raw field.

  **The distinction that decides each remedy:** *could a library author have prevented it with the tools
  kama gives them?* If yes it is an stdlib defect — fix the library, add a fixture, the language design
  is fine. If no it is a language hole and needs a compiler rule before the tag.

  | # | finding | class | size |
  |---|---|---|---|
  | ~~1~~ | ~~`extern fn` calls are ungated — double-free, zero `unsafe`~~ — **CLOSED**: calling an `extern fn` needs an `unsafe fn`, no scalar exemption | language | — |
  | ~~2~~ | ~~`addr(of:)` + an `UnsafePtr` field = general dangling-pointer factory~~ — **CLOSED**: `UnsafePtr` is contained; producing, holding and propagating one all need an `unsafe fn` | language | — |
  | 3 | ~~**`View::<T>.make` is a `public ctor`**~~ — **RESTATED, not closed**: the ctor is private and minting needs a `@viewable` grant, so safe kama cannot forge one. The length is still *trusted* — a type that owns the memory can hand out a truthful pointer with a false length | both | — |
  | ~~4~~ | ~~**view reseat** — nested block, loop body, and through a `ref View<T>` parameter~~ — **CLOSED**: a view may not be a `ref`/`out` parameter at all (fn, method and contract member), and a view local must root in a window, so there is no intra-function form left either | language | — |
  | ~~5~~ | ~~**resize invalidation** — `View` over a `DynamicArray` that grows~~ — **CLOSED**: the window rule forces the mint into a `borrow`, and the window freezes its host, so the `add()` that reallocs is rejected | language | — |
  | ~~6~~ | ~~**aliasing** — `bad(d: ref d, v: d.view())`, then `d.reserve(...)`~~ — **CLOSED**: a view argument roots through its receiver and is compared against every mutable argument *and* the receiver, which is where the sibling form `b.eat(v: b.view())` was hiding | language | — |
  | ~~7~~ | ~~**`reserve()` reallocs without bumping `mods`**~~ — **CLOSED, twice.** The counter now bumps in `growTo`, where the buffer actually moves, so every caller is covered at once. And `foreach` became a WINDOW, so growing the container mid-loop is a **compile error**: the mistake is unnameable rather than trapped, and the four repros moved `tests/trap/` → `tests/xfail/`. The counter stays as depth for the `unsafe`/FFI paths no static rule sees | stdlib | — |
  | ~~8~~ | ~~**the compile-time foreach-invalidation guard is dead code**~~ — **RETIRED**: the guard is gone rather than revived; the borrow window is the mechanism that replaces it | language | — |
  | ~~9~~ | ~~one `unsafe { }` disables definite assignment for the whole function~~ — **CLOSED by construction**: `unsafe` IS the function now, so relaxing locals is correct, and `out` params stopped being relaxed at all | language | — |
  | 10 | **an uninstantiated generic body gets no analysis at all** — unsafe gate, escape check, moves, definite assignment all deferred to instantiation. A package author ships `check`-green code and consumers get the errors | language, not UB | M |
  | ~~11~~ | ~~narrowing `cast<int8>(300)` → 44, silently~~ — **CLOSED**: a CONSTANT that does not fit is rejected at compile time, and a RUNTIME one now traps in every build, with `truncate<T>` (low bits) and `try cast<T>` (`Optional<T>`) as the two escapes. Record in [SPEC.md](SPEC.md) | wart | — |

  **Confirmed defended, by probe not assumption:** every arithmetic class (div0, mod0, `INT_MIN/-1`,
  shift width, float-cast, signed overflow — trapped in *every* build, `-fwrapv` in release); bounds on
  array/list/string/substring/view-index/view-slice/negative-index; dangling place-returns; use-after-move
  in a loop; the `export` rule under nesting; `parallel_for`'s write-capture rule, which sees a write made
  through a captured raw pointer; and `unsafe` does **not** leak into a generic body.

  **`type view` was the unfinished design under 3–6, and it SHIPPED** — a view answers *which* container
  it windows (the mint: a private ctor plus a `@viewable` grant) and *how long* the window stays open (the
  `borrow` scope). See [SPEC.md](SPEC.md#slices--spans--viewt-). Finding ③ is restated rather than closed,
  in the Working order above; ⑧'s dead guard is retired.

  What the survey behind it found, kept because it is the cost basis and not a recap: of 59
  `.view()`/`.slice()` sites only **11** are outside `tests/`, there are **zero** long-lived views in
  `lib/`/`prelude/`/`examples/`/`bench/`, and `examples/webgpu` holds no kama `View` at all. Of Rust's four
  borrow abilities, three cost kama nothing it uses; the fourth — storing a borrow in a struct — is
  *already* forbidden and worked around with borrowed raw-pointer fields: **37** `UnsafePtr` fields across
  the **18** `type view` declarations, of which 11 are `modsp` mutation counters and **26** are borrowed
  data pointers. That is finding ②'s shape, and it is why containment had to key on the type.

  **The remedy shipped: `unsafe fn`.** `unsafe` now marks the BODY (C#'s meaning, not Rust's), calling one
  is unrestricted, `extern` is gated at the CALL, and any expression, declaration or binding whose TYPE is
  or contains `UnsafePtr` may only occur inside one. Findings 1, 2 and 9 are closed; the record is in
  [SPEC.md](SPEC.md#what-requires-an-unsafe-fn--the-decision-table) and [GOALS.md](GOALS.md) §3a.

- **The unsafe seam — no `null` in safe kama.** Its own campaign, agreed while the `slot` work was in
  flight (which is where its customers came from: eight buffer-realloc sites now carry `= null` field
  initializers). **Two thirds shipped**: the raw pointer is spelled `UnsafePtr<T>`, and `null` is now
  rejected for every safe type in the STORE direction (declaration, field default, assignment) as it always
  was for `== null` — which is what "no `null` in safe kama" was actually asking for.

  What is left: a compiler-emitted **debug null trap** at the two `_inUnsafe` deref gates
  (`src/kama.cemit.cpp`, the raw index read and store), and `Optional`-returning **FFI wrappers** in
  `lib/std/ptr/`, its natural home.

  **⚠️ Measured by the boundary sweep: there is no memory-safety justification left for going further,
  and specifically not for the `Optional<UnsafePtr<T>>` proposal.** Three things came back from probing:

  - **Nothing null-shaped reaches a binary.** Dereferencing an `UnsafePtr` already requires `unsafe`, so
    null-deref UB is already inside the region the guarantee concedes. The type-keyed rule leaves **six**
    positions uncovered — call argument, `return null`, `Optional::Some(value: null)`, a module static, a
    field lvalue, an element lvalue — and all six pass `kama check`, but **clang rejects every one**
    (`error: incompatible pointer to integer conversion … from 'void *'`). They are therefore an instance
    of the tracked *"`kama check` does not type-check expressions"* gap, landing on the LSP and on an AI
    agent verifying its work — not on a shipped program.
  - **`Optional` has no unwrap, and one cannot be added today.** `match` is the only way to open it —
    there is no `?` operator and **no method on `Optional`**, because a generic enum is a
    monomorphization template and rejects members (`src/kama.cemit.cpp:10267`). `unwrapPtr` is monomorphic
    for exactly that reason, and the compiler emits calls to it directly into C at **nine** sites, so its
    signature is a hard dependency of the `new`/`try new` lowering. Pervasive absence without an
    ergonomic unwrap means a `match` at every one of ~30 stdlib sites.
  - **The niche optimization is not a layout tweak.** `Optional<T>` is tag-then-union
    (`emitVariantStruct`, `:14472`) with no layout special-casing anywhere; the tagged shape is
    constructed and destructured *literally, by field name*, at ~25 emission sites. (One genuine upside if
    it were ever done: `Some` is tag 0, which today forces an explicit reset of zero-initialized
    `Optional` fields at `:15387` — `NULL == None` would delete that.)

  **So: `UnsafePtr` containment, above, is the cheaper and more direct route to the same goal.** Every
  genuine `null` in the tree (75/75) targets an `UnsafePtr`; if `UnsafePtr` can only be handled inside
  `unsafe`, `null` is confined by construction — no `Optional`, no niche opt, no unwrap ergonomic, no
  token deletion, no `tree-sitter` change. What remained was the honest FFI surface — **shipped `0.9.255`** as
  `std::ptr::nonNull` (SPEC § *`unsafe fn` — raw pointer memory access*). The count was **7**, not the 8 the row said: `kama_channel_new`
  never returns null (`kama_channel.h` panics on allocation failure), so it is honest as declared, and making
  it fallible would be a `Channel.bounded` API decision, not a seam fix. Three of the seven were UNCHECKED
  before the row (`Shared.adopt` wrote through an unchecked `malloc`; the process argv/envp builders handed an
  unchecked `calloc` result to a writer) — the row's payoff. `fopen`/`dlopen`/`getenv`/`realloc`/`mmap` are
  not declared at all, since kama routes them through `kama_*` seams that already return `bool` + an
  out-param or a `Result`. ⚠️ `lib/std/memory/*` is EMBEDDED in the compiler as prelude modules and cannot
  import a disk module (`import { std::ptr::nonNull }` there reports "does not export"); `Shared.adopt` reads
  its null through the prelude's own `GlobalAllocator.allocate` instead.

  **The `unsafe UnsafePtr<T> p = null;` field modifier and the token-level `null` ban are dropped**, and the
  reason is worth keeping: both existed to force raw-pointer declarations to be greppable, and the rename
  already did that by the type's own name. The `null` rule that shipped is keyed on the **declared type**
  instead of on lexical context, which is strictly better — it needs no new grammar and no lexical region
  around a declaration, and its blast radius across `lib/` and `prelude/` was
  **zero**, because every real `null` in the tree already targets an `UnsafePtr`. Reopen only if a case
  appears that the type-keyed rule cannot express.
  - **It no longer owns iterator laundering — that shipped on its own.** The two were folded together on
    the argument that the honest fix *was* the seam, because making the iterators `type view` "is not a
    local fix": `Iterable<T>.iterator()` returns a *contract value*, so the iterator would box (heap
    ownership of a borrow), rejecting `View<T>`'s conformance and taking `sort` with it. **That premise was
    wrong.** `foreach` never dispatches through the contract — `emitForeachIterator` resolves
    `iterator()`/`iterMut()` structurally and emits direct monomorphized calls, and `sort` takes a `View<T>`
    directly, not an `Iterable`. Nothing boxes on that path, so all 17 borrowing iterators became `type view`
    with **zero compiler changes**. What is left for the seam is what it was always really about: saying that
    a raw pointer is raw.

- **`kama check` does not type-check expressions — so it reports OK on code that will not build.**
  `int32 x = "oops";` passes `check` and exits 0; only `kama build` rejects it, via the C compiler
  (correctly located, since the emitted C carries `#line`). What `check` *does* catch is name
  resolution, unknown functions/methods/types, named-argument mismatches, and ownership/move and
  serde analysis — a useful fast subset, but not the verdict its name suggests. It also owns the last
  residual of the `export { … }` rule: every position that *names* a type is checked, but an expression
  that never names one (`v.iterator().next()`, reaching a non-exported iterator through inference) is not a
  declaration at all, and catching it means knowing an expression's type. This matters most to
  the two consumers that surface `check`-class diagnostics and nothing else: an **AI agent** told to
  verify its work, and the **LSP**, which shows a clean buffer for a file that will fail to compile.
  Documented rather than hidden ([agents.md](agents.md), `usage()`, `agents/AGENTS.md`) and pinned by
  `tools/check-agents.sh`, which asserts the caveat still holds — so closing this gap will *fail* that
  guard and force the claim out of all three. Real expression type checking in the front end is a
  campaign, not a fix.

- **Contract refinement — one under-tested edge (clean workaround).** `type contract Child … implements
  Parent` works for dispatch, but was exercised mainly with scalar-param parents. Remaining: a concrete type
  implementing the child gets **no parent-contract conformance thunk** — pass it where the parent is expected
  only if it *also* spells `implements Parent` — and a child-contract-**value** → parent-contract-param upcast
  is unsupported (dispatch *through* the child to inherited methods works). Trivial workaround, used in
  `lib/std/net/stream.kama`; the fix is to auto-emit parent thunks for refining-contract implementers.
  *(The generic-instance param edge is fixed — an inherited slot's signature is now rebound to its
  parent-resolved absolute spelling in `linkContracts`; fixture `tests/contract_refine_generic.d`.)*
- **Unicode module (post-1.0).** The shipped `string` core is UTF-8 bytes + `.chars()` codepoints with
  **ASCII** casing/whitespace; a later module adds Unicode-correct casing + whitespace, and an eager
  `DynamicArray<string>` collect for `split` (the lazy `Split` iterator ships today).
  **Grapheme-cluster segmentation belongs here too.** `substring` traps on a split *codepoint* and
  `floorCharBoundary`/`truncate` snap to one (SPEC § *Strings*), which guarantees valid UTF-8 but **not**
  visually intact text — a boundary cut can still split an `e` + combining accent, an emoji ZWJ sequence
  or a flag. Cluster boundaries are defined by UAX #29 and need the `Grapheme_Cluster_Break` property per
  codepoint, i.e. a data table — not a bit trick. A cheap partial version (range-checking the combining
  diacriticals) would be wrong for emoji, flags, Hangul and Indic while *looking* like a guarantee, so it
  is deliberately not shipped. Same stance as Zig, and utf8everywhere points at ICU for it; baking the
  tables into `std` would also contradict targeting MCUs under `--no-heap`.
- **Stdlib layering — 3 LOW-prio follow-ups.** The prelude-vs-`std::`-vs-primitive split is principled and
  documented in [FLOOR.md](FLOOR.md) § "What is floor, and what is an `import`"; nothing is mis-placed. What
  is left, none of it blocking:
  - **`Atomic` needs no pthread but rides in `std::concurrent`**, which does — so lock-free-without-threads
    is unreachable. Split it to a `std::concurrent::atomic` leaf, or promote it as an always-available seam.
    Only matters once the multicore-MCU track starts (§5).
  - **`std::gpu` has no kama module.** The `kama_gpu.h` seam exists but programs `extern` the WebGPU C API
    raw; an idiomatic wrapper is library work on the engine track (§8).

  Decided NOT to add a convenience-import of the common containers — explicit per-symbol imports stay.
- **Format/interpolation follow-ups (on the shipped `std::fmt` substrate).** Interpolation, format specifiers,
  `@generate(Formattable)`, and tagged strings all ship (SPEC), on every subject including a generic and an
  `enum` (0.9.246 — see *Derive follow-ons* above). Settled by the audit (2026-09-07). Shipped 0.9.226: combining a
  base marker with width/flags (`${n:08x}`), a custom fill character, center-align (`^`); a `${x:?}`-routed
  `@generate(Debug)` (spec hook already exists);
  per-derive `@skip(Formattable)` / `@skip(Serializable)` for redaction (today `@skip` is one shared boolean —
  parameterize `FieldInfo::serSkip` to a per-derive set when a concrete case appears); and tagged-string
  *type-preserved params* (Model B — each hole keeping its static type into the params list, `html` returning
  a distinct `SafeHtml`). Regex is a separate campaign. `string + <number>` stays a compile error — a **non-goal**: `"${x}"` is the one way to render a value (SPEC § *Interpolation*), and the emitter's message says so.
- **Full `expose` (2.0).** The minimal `expose fn` free-function C-ABI boundary ships today (SPEC + §8
  hot-reload); the **full `expose`** — richer wasm module exports + the scripting host interface — stays 2.0 (§7).

  What is actually missing on the wasm half, measured 2026-08-23 rather than assumed. An `expose`d
  function **is** a real wasm export (`WebAssembly.Module.exports()` lists it), so an embedder that
  instantiates the `.wasm` directly can already call it — and never runs `main`. What does **not** work is
  reaching it off the generated JS as `Module._add`: that needs `-sEXPORTED_FUNCTIONS`/`-sMODULARIZE`,
  and kama emits a PROGRAM (shebang, runs `main`, exits), not a library. `tests/expose_basic.kama` claimed
  the `Module._add` form worked; it never did, and its comment now says so.

  ⚠️ **This row owns a constraint from `0.9.63`.** Every wasm build now sets `-sEXIT_RUNTIME=1`, because
  without it node's graceful teardown deadlocks against V8's background threads (see the comment at the
  flag in `kama.driver.cpp`, and `run_tests.sh`'s watchdog history). A *module* artifact must keep its
  runtime alive after `main`, so shipping this work means making that flag conditional again — on the
  artifact **kind** (program vs module), never on which library the program happens to use, which is the
  keying that was wrong before.
- **Derive follow-ons — SHIPPED `0.9.234`–`0.9.246`** (SPEC § *Derives* carries the surface; this entry keeps
  only the verdicts, which is what a shipped row leaves behind). Every subject derives now: a generic type
  **per instantiation** and **conditionally on its fields** (the implicit conditional conformance Rust writes
  as `impl<T: PartialEq>`; the use site names the field that disqualified an instance), a tagged `enum` and a
  payload-less one **per tag**, and a generic `enum` by both rules at once. The prelude's `Optional`/`Result`
  carry `@generate(Equatable, Hashable, Formattable)`.
  Four **non-goals** were settled on the way, each because something already answers the need:
  - `of`/`zero` on an `enum` — a variant IS its own memberwise constructor (`Shape::Circle(r:)`), and `zero`
    names no variant. Refused, with that as the message.
  - `Serializable` on the prelude's `Optional` — an `Optional` FIELD already has a wire form (the inline
    `null`/value case), so a derived `{"tag":"Some",…}` would be a second form for one type.
  - A `Copyable` derive — a value/view copies by kind, and a resource's `copy` ctor is an ownership decision
    no field walk can make (a memberwise copy of a raw handle double-frees).
  - A payload-less enum's wire form is a bare string (`"Green"`), not `{"tag":"Green"}` — it is a name, not a
    record, and the shape is fixed per TYPE so a schema still reads cleanly.
  ⚠️ Two lessons worth more than the feature. The refusal on an enum ("accepts only Serializable,
  Deserializable") was not a judgement about enums but a SECOND attribute parser holding a stale copy of the
  accept-list — the four newer derives were not unsupported there, they were unknown to a copy nobody grew.
  And the first shape a newly-legal feature opens is the one no fixture covers: `@generate` on a generic
  became legal at `0.9.239` and a derived template that *nobody instantiates* then failed to compile, because
  the uninstantiated-template probe asked an opaque `T` whether it conformed and three rules answered "no"
  (`0.9.240`).
- ~~**`INT32_MIN` has no direct spelling.**~~ **Fixed.** `-2147483648` folds the negation into the
  literal at parse time (Rust's rule): the magnitude is one past INT32_MAX so the literal alone is
  rejected, but under a unary minus it fits exactly. It previously did not work at all and for an
  unrelated reason — the emitter wrote `--2147483648`, which the C compiler reads as a pre-decrement.
  Guarded by `tests/int_literal_min.kama`.

  ~~**Same family: a SUFFIXED literal is not range-checked against its own suffix.**~~ **Fixed** as
  milestone 5b-C, the same rule at every width: a magnitude one past the maximum parks and the `MINUS`
  claims it, anything larger is rejected at the literal. The brief filed this as a latent hole because the
  corpus is clean — 885 suffixed literals, none out of range — and that was half right. `300i8` really was
  only latent. But the NEGATIVE boundary was a live bug: `-128i8` narrowed to -128 at parse time, the
  minus then negated an already-negative node, and the emitter wrote `(--128)`, which C reads as a
  pre-decrement. INT8_MIN/INT16_MIN/INT32_MIN/INT64_MIN had no suffixed spelling at all, and `kama check`
  passed the file — only clang objected. Guarded by `tests/int_literal_suffix_min.kama` and three
  `tests/xfail/int_literal_suffix_*` fixtures.

- **Unresolved type names — one residual: GENERIC ARGUMENTS.** Declared type names are now checked
  (`checkDeclaredTypes`, a single-visit walk at the tail of `collectProgram`), so a misspelled or unimported
  type in a parameter, return, field or variant payload is a kama-level error instead of a C-level
  `undeclared identifier` against generated code. What is still unchecked is a type name *inside* a generic
  argument — `DynamicArray<Bogos>` — because `checkTypeResolves` early-returns on `type->genericArg`.
  Recursing into `genericArgs` (as `addTypeRef` does in kama.query.cpp) is the natural phase 2, but it
  widens the surface onto comptime size expressions, defaulted allocator args and bounds, so it wants
  its own sweep. Guarded today by `tests/xfail/unknown_type_{local,param,return,field,method_param,
  variant_payload}` + `unimported_type_param`, and by `tests/decl_type_check_guards.kama` for the three
  shapes the pass must NOT reject (a generic free fn's own params, `This`, a `sig` used before its file).
- **`std::net` — IPv6 and UDP multicast.** `IpAddr` has a `V4` arm only ([`lib/std/net/addr.kama`]), left
  deliberately as an `enum` so a `V6(...)` arm adds without reshaping `SocketAddr` or any call site.
  Multicast join/leave (`IP_ADD_MEMBERSHIP`) is likewise unbuilt — broadcast covers LAN discovery today.
  Both are ordinary socket-option work on the shipped seam.
- **Contract conformance now IS checked when a value is bound to a contract** (0.9.105) — recorded here
  only because the shape of the miss is worth not repeating. The argument hand-off skipped the KIND rule
  for a contract destination, correctly (a contract admits every kind by design, which is what makes
  `hashVia(h: 22)` over a primitive a shipped feature) — but nothing was put in its place, so the one
  position that skipped the kind check checked nothing at all. ⚠️ And the diagnostic that DID exist, in
  local-declaration position, was the emission cascade's fall-through `else`: it fired only for a value
  `exprClass` cannot type, so it caught a plain enum and let every non-conforming CLASS through to the
  branch above it, which emitted `Plain__as_Hashable` for clang to discover did not exist. **A check that
  only sees what its neighbours could not classify is not a check** — and it read as one for as long as
  nobody wrote the class case down. Pinned by three `tests/xfail/contract_*_nonconforming.kama`.
- **Class-to-class mismatches are a DIAGNOSTICS defect, not a soundness one.** `D d = c;`, `return c;`
  where `D` is declared, and `take(d: c)` all fail — but as a *C-level* message about mangled names, on a
  kama line. C never assigns between two struct types, so clang refuses them and nothing wrong-typed
  reaches a running program. Deliberately scoped OUT of the type-identity rule (0.9.103) for that reason.
  ⚠️ **`return this;` in a fallible ctor** (instead of `return Result::Ok(value: this);`) is the same
  defect reached from the ctor path: it emits `__ret_0 = self;`, a `G*` into a `Result<G,Err>`, caught
  only by clang.
- **`UnsafePtr<A>` into `UnsafePtr<B>` is unchecked by kama.** Probed 2026-08-28: `UnsafePtr<int32>` into
  `UnsafePtr<int64>` compiles with a clang *warning* and runs, which is a genuine width hole (eight bytes
  read from a four-byte allocation). It sits inside `unsafe fn`, which is the sanctioned trusted region,
  and the two C spellings differ so clang does see it — hence tracked here rather than in the identity
  rule, which covers only types that SHARE a spelling.
- **Capturing closures — sized, not scheduled (audit verdict, 2026-09-07).** The shape a UI event table wants
  today is a generic functor: `type contract Handler<E> { fn void call(E e); }`, one `type resource` per handler
  carrying its captures as fields, stored as `Owned<Handler<E>>` in the table — the comparator twin
  [SPEC.md](SPEC.md) shows under *Ordering comes from a contract*. A closure is SUGAR over exactly that: a lambda
  expression with an explicit capture list (`give`/`copy` per capture — no lifetime tracking, so by-move or
  by-`Shared` only) lowering to a synthesized resource plus the conformance, with the sink staying a contract
  (inlinable, monomorphized) rather than a function pointer. Size **L**: grammar, the capture-ownership rules,
  a synthesized type per lambda, and the diagnostics for a capture that escapes. The functor answers the need;
  the sugar is wanted when a real event table is written by hand and its boilerplate is measured — not before.
  **Measured (2026-09-07, `tests/functor_event_table` and `tests/functor_by_ref`):** both forms compile and
  dispatch today with nothing added — the stored table through `Owned<Handler<E>>`, and the lent visitor
  through a `<V: Visitor<T>> ref V` bound with its state read back after the call, which is the case Rust
  spends borrowed captures on. The boilerplate a closure would erase is four lines per handler (the type
  header, one ctor, the conformance, a field per capture) around one line of behaviour; what it would NOT
  add is a borrowed capture, since neither spelling has lifetime tracking. Two verdicts written with it:
  the prelude ships no `Callable` family (an arity ladder without variadic generics; a callback signature
  is the library's contract), and the sugar stays deferred by the maintainer's ruling — SPEC § *Ordering
  comes from a contract* carries the idiom.
  (The row that sat in NOW as "No capturing closures" is gone: this is its verdict.)
- **`int128` — non-goal; the wide product shipped (0.9.231).** `__int128` exists in clang and gcc on 64-bit
  targets only (not MSVC, not gcc on thumbv6m), so a kama `int128` would be a numeric type that exists on some
  targets, which the fixed-width position forbids. What reached for it — a `Fixed<int64>` backing's
  intermediate product, Lemire's unbiased range — needs the PRODUCT: `std::num::mulWideU64`/`mulHighU64`/
  `mulWideI64`, four 32-bit limb multiplies in plain unsigned arithmetic on every target. Both consumers are
  now *optional* follow-ons, recorded beside their code.
- **Explicit SIMD — SHIPPED 2026-08-31**, all three stages. The record of what the surface IS lives in
  [SPEC.md](SPEC.md) (*Explicit SIMD*); this entry keeps only the MEASUREMENTS behind it, because each one
  cost real time to obtain and every one of them contradicted an assumption someone held first.

  - ⚠️ **`std::math` already auto-vectorizes on native, and beats hand-written SIMD.** `fadd.4s` loops,
    and clang de-interleaving AoS to SoA so `dot` and `Mat4.transform` run four at a time. A hand-written
    explicit-SIMD cross product measured **65% SLOWER** than the scalar source for exactly that reason.
    So `Simd` is **not** "SIMD for a language that had none", rebuilding `std::math` on it is rejected on
    measurement, and the docs say so where an author would reach for it.
  - ⚠️ **`ext_vector_type` is a silent miscompile under gcc** — ignored with a warning, leaving a
    ONE-LANE scalar. `vector_size` is the only portable spelling; `__builtin_shufflevector` works on
    both, so only the type ever needed a seam.
  - ⚠️ **gcc has no `__builtin_elementwise_*`**, and treats the name as an *implicit function
    declaration* — a warning, the same shape as above. Elementwise ops are written as per-lane loops,
    which both backends fold to the branchless vector form.
  - ⚠️ **`-fno-math-errno` is what lets a per-lane libm loop vectorize.** With it, `sqrtf` per lane folds
    to `fsqrt v0.4s` on gcc and clang; without it neither folds. macOS defaults to it and Linux does not,
    which is how a host reading came to disagree with the container's. **The driver passes it on every C
    compile since `0.9.256`** (kama reads no `errno` after a libm call; results are bit-identical), and
    `check-simd-type.sh` mirrors it. `floor`/`ceil` fold (`frintm`/`frintp`) with or without it — only
    `sqrt` has the errno side effect. `sqrt`/`floor`/`ceil` are NOT in `kama_runtime.h` for a different
    reason — it is freestanding, and they need libm — so they are `KAMA_SIMD_MATH` in `kama_math.h`,
    emitted beside a FLOAT lane batch's `_FUNCS` (an integer batch has no such methods; SPEC § *Explicit
    SIMD*). Both guards assert the fold by opcode name: `fsqrt|frintm|frintp` / `sqrtps|roundps` on native,
    `f32x4.sqrt|floor|ceil` on wasm.
  - ⚠️ **UBSan does not instrument vector arithmetic.** Same build, same flags: a scalar `int32 MAX + 1`
    trapped and the identical addition in a lane wrapped. The overflow check for signed lanes is emitted
    by the compiler because nothing else supplies it.
  - ⚠️ **A mask must carry `T`.** A comparison's lane width follows its OPERAND's — f32x4 gives 4-byte
    lanes, i16x8 gives 2-byte — identically on gcc 13.3 and clang 18, so a bare `Mask#(N)` has no C type.
  - ⚠️ **Probe design, which went wrong three times across two sessions.** A SIMD claim is invisible to
    exit codes, so the instrument is everything: never measure at an ABI boundary (AAPCS64 passes
    `struct{float x,y,z,w}` in four separate registers), never let the kernel be constant-foldable or
    dead, keep every lane of the result live or the compiler deletes the others, and remember that Apple
    writes `fadd.4s v0, v0, v1` where GNU writes `fadd v0.4s`. The rules live in the headers of
    [tests/support/simd_probe.kama](../tests/support/simd_probe.kama) and
    [simd_type_probe.kama](../tests/support/simd_type_probe.kama), beside the probes they constrain.
  - ⚠️ **A guard's negative control depends on what it measures.** `check-simd-native.sh` can use `-O0`,
    because it measures AUTO-vectorization. `check-simd-type.sh` cannot: an explicit vector type emits
    vector instructions at every optimization level, so its control is a scalar-only program the pattern
    must not match.

  Not built, and each is a decision rather than an omission: **widths above 128 bits** wait for the
  CPU-tuning knob (§9) — no AOT language ships wider lanes without a build flag, so this is parity, not a
  gap; **per-ISA intrinsics** are a declared non-goal, being the half every surveyed language keeps
  `unsafe` or experimental. (**`sqrt`/`floor`/`ceil` on a lane batch** shipped `0.9.256` through the
  `kama_math.h` seam, with the wasm leg's opcode-level proof — the `-fno-math-errno` bullet above.)

  ⚠️ **On the 1.0 tag:** the row is done, so the question it raised is closed — nothing here was
  source-breaking, and the surface is additive.

- **UBSan's `function` check is disabled suite-wide, for a REASON — not an oversight** (`run_tests.sh:60`).
  It is a false-positive suppression, not a masked bug: kama's dispatch stores every slot as
  `Ret (*)(void* self, …)` and calls the concrete `Ret C__m(C* self, …)` through it. That type-erased
  `self` is ABI-identical — it is how essentially all C OO dispatch works — but the `function` sub-check
  enforces exact function-pointer type identity, so it would flag *every* contract call. Every other UBSan
  check (integer overflow, null, bounds, alignment, …) and all of ASan stay on. The residual risk is narrow
  and real: a genuine slot/signature mismatch is not caught *by UBSan* (the emitter builds both sides, so
  one usually fails to compile). If that coverage is ever wanted back, the route is emitting a per-slot
  typed thunk — `static Ret C__m__thunk(void* self, …)` that casts and calls — which makes the pointer
  types exact and lets the check be re-enabled — but that buys nothing (the emitter generates both sides
  of a slot from one declaration, so a real mismatch fails to compile) and adds an indirection to every
  dynamic call, which is the wrong trade for the embedded and hot-path targets. **DECIDED 2026-08-12: the
  exemption is permanent, and [SPEC.md](SPEC.md) now says so** in the numeric-safety/sanitizer section,
  including the guidance that a user building under `-fsanitize=undefined` should pass
  `-fno-sanitize=function`. Closed — kept here only so it is not re-diagnosed as a hole a third time.
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts with "one way to do
  a thing," and **named parameters** already cover the disambiguation overloading is usually reached for.
  **Operators are the sanctioned exception** — a type may carry several `operator*` distinguished by operand
  type (`mat*vec`, `mat*mat`), matching C++/C#/Rust. Reopen only if a concrete case shows named params can't
  express it.

- **The release tier's signed-overflow rule was toolchain-dependent — FIXED 0.9.125.** Kept here because
  the shape recurs: a flag interaction that two of three toolchains agree on reads as settled. The driver
  passed `-fsanitize=signed-integer-overflow` in BOTH tiers, keeping it in release only for
  `INT_MIN / -1` (which `-fwrapv` does not define), on the assumption that `-fwrapv` suppresses it for
  the ordinary ops. ⚠️ **Measured: that holds on Ubuntu clang 18.1.3 and gcc 13.3, and NOT on Apple clang
  21.** So a `--release` `int32 MAX + 1` trapped on macOS and wrapped on Linux from one source — and
  worse, every signed add carried `adds; b.vs; brk` and every multiply `smull; cmp; b.ne; brk`, against a
  bare `add`/`mul` on Linux. The performance invariant at the top of ROADMAP.md was false on macOS.
  Fixed by making the sanitizer debug-only and emitting the `TYPE_MIN / -1` check
  (`kama_sdiv_i32`/`_i64`, tested on the OPERANDS because `INT64_MIN / -1` overflows a result check).
  ⚠️ It went unnoticed because nothing could see it: `tests/trap/` builds debug only, and
  `tests/trap/intmin_div.kama` even STATED the release behaviour in its own comment without ever testing
  it. `tools/check-release-arith.sh` now asserts the semantics *and* the zero cost in a real release
  build.

- **Found by the first external project on kama** (a game port, 2026-08-31). Six of its reports are now
  fixed: a multi-module library not being consumable as a dependency, the output being named after the
  alphabetically first source file, absolute dependency symlinks, three stale claims in the WebGPU
  example, and — in `0.9.128` — the two recorded below. They are kept together because their provenance
  is the point: every one was found by someone *using* the language rather than by the corpus, and not
  one of them had a fixture that could have caught it. ⚠️ **Two of the six turned out to be bigger than
  their report** (the `comptime` export was a whole unwired subsystem; the frame loop was a seam gap, not
  an example typo), which is the argument for probing a user's report rather than patching its sentence.

  - **A module-scope `comptime` constant could not be exported — FIXED 0.9.128.** Kept as a record
    because probing it found something much larger than the report, and the shape recurs. KB-3 was
    filed as "naming a `comptime` in `export { }` is rejected", and the plan was to decide between
    fixing the doc and fixing the compiler. ⚠️ **Neither resolution was available: the probe found the
    subsystem had never been wired at all, and returned FIVE different answers for one declaration.**
    Same file: works. Sibling file in the same module: `kama check` says OK, `kama build` emits invalid
    C. Another module, qualified: same — check OK, clang fails. `import`: "does not export". `export`:
    "no such top-level declaration".
    - **The cause.** `declFileOf` and the export/import membership tests consulted seven registries,
      none of which hold a module-scope declaration — so the file rung could not see a module variable
      and waved through every cross-file reference. And a `comptime` lowered to `static const` in its
      declaring unit's own `.c`, so no other translation unit had anything to link against. Both halves
      were invisible to `kama check`, which folds the program into one unit.
    - **The fix.** A module `comptime` is now emitted into the shared header (`static const` — one
      addressable copy per TU, which is right for an immutable value and violates no ODR), joins the
      export/import membership sets, and resolves through `resolveModuleVar` — the same alias/qualifier
      search `resolveFuncImpl` does — at all three of its use sites, so an imported or module-qualified
      constant can also size an `InlineArray` and be addressed. A mutable module `static` is now
      **rejected at the export list with its own sentence** and gets the file rung too, which turns the
      remaining silent miscompile into a diagnostic.
    - **One nuance left open, unprobed.** Only an *exported* constant moves to the header, so a private
      `comptime fn` table still lives in one unit — but an exported one is `static const` in every TU
      that includes the header. An ordinary constant is dead-stripped where unused; a `@section`-placed
      one on an MCU may not be. Genuinely optional, remedy decided (the audit, 2026-09-07): a `static const` per TU is what a `@section`
      placement needs anyway, and a single link-time copy — if ever wanted — is one definition plus `extern`
      declarations, not a retreat from the header.
    - ⚠️ **The lesson, which is the reason this entry stays.** The report named the narrowest visible
      symptom. Had it been taken at face value — add the name to one membership set — the export would
      have been accepted and the program would still have failed in the C compiler, and the fixture
      proving the fix would have been a single file, which is exactly the shape that could not catch
      any of this. **A cross-file claim needs a cross-file fixture** (`tests/mod_export_const.d/`).
  - **`examples/webgpu/triangle.kama` had an unbounded-allocation frame loop — FIXED 0.9.128.** The
    example now reads `surfTex.status`, treats `Occluded`/`Timeout` as skip-the-frame, reconfigures only
    once per `Outdated`/`Lost` invalidation, stops on a 600-frame failure streak, and **sleeps on every
    path that returns without presenting** — through a new `kama_gpu_sleep_ms` on the `std::gpu` seam
    (native sleep, web no-op, because the browser owns the rAF loop). The seam function is deliberately
    an unconditional sleep rather than `glfwWaitEventsTimeout`: a throttle that can return early on an
    event is not a throttle, and this function exists because a loop lost its only throttle. The record
    of what went wrong, kept because the shape is general: when
    `wgpuSurfaceGetCurrentTexture` yields no texture the example reconfigures the swapchain and returns
    **without presenting** — and vsync, its only throttle, applies only to a presented frame. So the path
    runs at unbounded rate, allocating a swapchain per iteration. ⚠️ The trigger is trivial and permanent:
    `Occluded` (a wgpu-native extension the example never consults, not one of `webgpu.h`'s six statuses)
    is returned with a NULL texture whenever the window is not visible — another window in front is
    enough. The reporter reached 15.6 GB resident and took a machine down through the kernel watchdog,
    twice. ⚠️ **The language half is the larger point, and it promoted a row** — see the safe `std::gpu`
    wrapper in [§8](#s8). Nothing in kama could have caught this: the allocation is inside wgpu-native,
    reached through `UnsafePtr` handles carrying no RAII, so there is no kama object, no destructor and
    nothing for `@noheap` to see. ⚠️ **But be precise about which half would have caught it** — the
    reporter's framing, and this entry's first draft, both said RAII, and that is wrong. RAII would not
    have helped: the leak is wgpu-native's swapchain, triggered by ignoring an untyped `status` int. What
    prevents *this* bug is a **typed acquire result** whose error enum names `Occluded`. RAII prevents
    the *other* leak in the same file — five hand-written `wgpu*Release` calls per frame, all on the
    happy path, which any later early `return` would leak. Two halves, two different bugs, and a wrapper
    wants both.
  - **Build settings did not propagate from a dependency — FIXED `0.9.165`-`0.9.168`.** See the
    as-shipped record below (*The build-settings campaign*).
  - **The `std::gpu` seam is half-built.** `kama_gpu_pump` calls `glfwPollEvents()` and **discards the
    queue** (the web pump is `{ return 1; }`), so there is no keyboard, mouse, wheel, pointer-lock,
    resize, focus or gamepad on either target; and the surface is configured at a hardcoded 512×512 with
    no way to learn the drawable size. A size accessor is a few lines. Whether the input *library* above
    that seam is kama's or an engine's is the more arguable half, but a window seam that polls events and
    throws them away is not finished. **Now split across two rows** — the size accessor and the handle
    RAII ride the safe-wrapper row, and input is its own (`std::input`, a peer of `std::gpu`). ⚠️ The
    arguable half has an answer now: the first engine built on kama **derives its own window** and
    duplicates ~60 lines of surface-derivation out of `kama_gpu.c` to get keyboard and mouse, which is
    the outcome a seam that throws its events away forces on everybody.

- **The first consumer's second audit (2026-09-01), triaged against this tree.** Everything below was
  REPRODUCED here before being scheduled — their report names the symptom, and three times running the
  shape underneath it has been different. Their doc is `../friendly-fire-department/docs/KAMA_GAPS.md`;
  they renumber it between audits, so find an entry by its text, never by a remembered KG number. They
  pin `0.9.132` and have verified `@noheap` transitivity by behaviour in their own tree.

  - **A contract member returning a type declared BESIDE the contract — FIXED `0.9.134`.** ⚠️ **It was
    FIVE sites doing the same partial swap, not one bug.** A contract's member signatures are rendered
    under the CONTRACT's own name-resolution scope — its imports, not the implementing unit's — and five
    places did that by hand, each moving three of `NsCtx`'s four fields and leaving `unitPath` pointing at
    the other file. That is not a cosmetic omission: `checkReach` decides the per-file import rung by
    comparing exactly that path against the file being walked, *precisely so it can tell "these imports
    belong to a different file"*. A stale path made the two agree, so `Thing` was judged against the
    contract's own imports — and a file does not import what it declares, so the implementer was told to
    `import { Thing };` a name its line 1 already imported. Only a member returning a USER type could
    reach it, which is what made it look like a type-system bug.
    - ⚠️ **Four of the five were found by fixing the first four.** The repro moved its diagnostic from
      `impl.kama` to `main.kama` twice as each site was closed — `contractSigOf`, `emitClassInterfaceVtables`,
      the refinement-thunk loop, `emitInterfaceTypes` (which had no reseat at all) and the
      contract-value return-type path. Each time the answer was to instrument `checkReach` and read
      `nsUnit` against `refFile`, never to guess the next one. They are one `ScopedContractNs` now, which
      also carries the diagnostic file — so a sixth site cannot get it wrong.
    - ⚠️ **The misattributed diagnostic was the same bug, not a second one.** It named one file's path
      with another's LINE (`impl.kama:9`, where line 9 of that file is a closing brace and line 9 of the
      contract's file is the member) — because the scope had moved and the diagnostic file had not. It
      cost the reporter the bisection; it cost one line to fix.
  - **A module `comptime` cannot be interpolated — FIXED `0.9.134`, and it was neither of those things.**
    Reported as comptime-specific and interpolation-specific; it is a MODULE-SCOPE name failing as a
    method RECEIVER. Interpolation only lowers `${X}` to a method call on the value, so a module `static`
    failed identically — the half nobody reported — and so did a direct `X.toString()`. One missing
    lookup: `receiverScalarCType` and `receiverTypeNode` consulted locals and bound `comptime` parameters
    but never `_moduleStatics`, though a module name resolves the way a module function does
    (`resolveModuleVar`). A module-scope name now behaves identically to a local at that site, verified by
    the two producing the same diagnostic for the same mistake.
    ⚠️ **The line number fixed itself.** "at line 1, column 0" was not a separate defect: resolution
    failed, so the error came from a node carrying the constant's declaration line. What was worth fixing
    on its own is the message, which was four words with no subject — it now names the receiver, which is
    the general rule that stops the next one costing forty minutes.
  - **A `fnptr` in a field or a module `static` cannot be CALLED — FIXED `0.9.134`.** ⚠️ Filed as four
    failures; two did not reproduce (binding into both places already worked), so it was one defect with
    two arms: `emitInvocation` recognised only a bare LOCAL of signature type, so a `static` call fell
    through to "call to unknown function", and a field call reached METHOD dispatch and was reported as a
    missing method. Both arms added; the field arm is checked last, after every real method has failed to
    resolve, so a field can never shadow a method. The callback registry — install now, dispatch later,
    re-bind in between — now has a spelling ([tests/fnptr_stored.kama](../tests/fnptr_stored.kama)), and
    SPEC's `fnptr` section says so.


  - **The real-time cluster stopped being an argument and became a number.** Because kama cannot run on
    the CoreAudio thread, their synth runs on the frame loop and feeds the device through a ring — so
    stall tolerance IS latency, and the two cannot be traded. A macOS session logged **326 underruns** at
    a 170 ms queue; deepening it to 683 ms fixed occlusion outright and still left **~290 during window
    drags**, because `glfwPollEvents` blocks inside a nested run loop for the whole drag while the
    producer never runs. The audio thread keeps running throughout — that is *how* the underruns were
    counted. Their conclusion, and it looks right: moving the synth into the device callback "fixes the
    case completely, and nothing else does", which needs the foreign-thread row to be safe at all, the
    `@noheap` transitivity that shipped for the guarantee to be real, and the panic-policy row before it
    can ship — a bounds miss in a mixer currently aborts the process from a thread nobody can see.
  - **Build settings — SHIPPED `0.9.165` through `0.9.168`.** Their two complaints and one more this
    campaign found, with the reasoning that is worth not re-deriving:
    - **A project tier had to come first.** `cflags`/`ldflags` existed only under `select.TARGET.<NAME>`,
      and a target is matched BY NAME — so `--target aarch64-linux-gnu`, an anonymous triple, matched no
      entry and got none of them. Decisively: a DEPENDENCY cannot know how its consumer spells the
      target, so propagation alone would have been half-dead on arrival.
    - **Per-manifest resolution, then concatenation.** Each manifest resolves under its own precedence
      and only then are the lists joined, because `link`-REPLACES applied globally would let a dependency
      delete the consumer's `-lm` — adding a dependency could break your link.
    - **Read from the `.kama/deps` VIEW, not from `kama.lock`**: the view is flat and already transitive,
      and `kama.local.json` `overrides` repoint it without touching the lock, so a lock-driven walk could
      read settings from a dependency other than the one being compiled.
    - **The negative half is the load-bearing one.** A rule that propagated everything would pass every
      positive test. A dependency cannot reach the consumer's `cc`/`ar`/`sysroot`/`runtime`/`subsystem`,
      cannot impose `no-heap` (it changes what compiles, program-wide), cannot demand a `webgpu` SDK
      download, and cannot contribute a RELATIVE `-I` (it would resolve against the consumer's working
      directory and silently find the consumer's own `include/`).
    - **`emSettings` is an object, and the value's SHAPE decides its kind** — an array unions, a scalar
      is last-wins with the project winning. An array of raw `-sFOO=1` strings would have been `cflags`
      with extra steps, and the merge is the entire point: emcc is last-wins, so kama's own
      `-sEXPORTED_RUNTIME_METHODS` (emitted after the project's `cflags`) silently beat any project that
      set the same key — while simply moving the project later would have dropped the two names the
      stdlib's JS glue needs. ⚠️ Two dependencies disagreeing on a scalar must be recorded and reported
      AFTER the project merges: reporting during the walk refuses a manifest that had already settled it.
    - ⚠️ **`OUTPUT=OBJECT` with more than one translation unit was UNREACHABLE**, found writing the
      guard. The arity check sat in the single-invocation arm and counted `cFiles`, but two inputs mean
      `nJobs > 1`, so such a build took the per-TU path, compiled each input, and "joined" them with a
      command still carrying `-c`. No diagnostic, and an artifact nobody can use. Reachable before this
      campaign, via `needsGpu`.
    - ⚠️ **The reproducible-float fixture would have been VACUOUS.** It reproduces only under
      `--release`: in debug, `a * b + c` is emitted as `KAMA_ADD(KAMA_MUL(a, b), c)` and the
      overflow-checking macros already break the expression clang would have contracted. `run_tests.sh`
      builds every fixture in debug, so the planned `tests/*.d` fixture would have passed identically
      with and without the key. The oracle lives in `tools/check-buildsettings.sh` instead, and detects
      its own vacuity on a host with no FMA.
    - **The window/framework sub-complaint is answered by the project tier, not by hoisting kama's
      list.** A project with its OWN window seam now writes its own `link` + per-target `ldflags`, which
      is the right outcome: kama's hardcoded GLFW/framework list exists for a program that externs
      `kama_gpu.h`, and a project that does not is not entitled to track it.
    - What is left is the C++ half of `csources`, which is its own row.

- **`csources` compiles C, not C++ — the row, and the BUG that was hiding behind it (KB-16).** The
  consumer carried this as one open bug for eight releases while kama reported its queue empty, and the
  reconciliation is that it was **two defects** and kama held only one. Settled by reproducing it,
  2026-09-07:
  - **The gap (this row, still open).** A `.m`/`.cpp`/`.cc`/`.mm` entry is refused BY NAME, with the two
    obstacles the diagnostic already states: every input shares one flag prefix (`-std=c11` plus the
    C-only warning promotions), so a C++ TU needs its own, and a C++ link needs the target's C++ runtime
    (`-lc++` vs `-lstdc++`), a per-target table kama does not have. Objective-C is a third case — one
    platform's language, and `csources` is project-level with no per-target tier to exclude it. A
    per-entry or per-target language tier closes all three; a fix for Objective-C alone closes none,
    because the C++ half is what keeps the consumer's hand-written Makefile alive (48 lines when this was
    filed, 556 after their M4).
  - **The bug (FIXED, `0.9.238`).** The only way to say "compile this as Objective-C" was `-x objective-c`
    in `cflags`, and a project's `cflags` were reaching the LINK command, where `-x` is a sticky mode flag
    that applies to the `.o` inputs. Every consumer's link died lexing Mach-O bytes as source. That is
    wrong independently of this row — a link consumes objects and compiles nothing — and it is why the
    workaround was fatal rather than ugly. Pinned by `tools/check-buildsettings.sh`.
  - **The lesson for triage, since it cost eight releases:** a report that is half gap and half bug gets
    counted by whichever half the reader is holding. Split it on arrival.

  - **Declared NOT ours, and they agree** — the audio backend, WebGPU binding breadth, their RFC6455
    framing, module statics being per-isolate (correct behaviour), and a PATH entry that is a directory
    breaking `make` in the emscripten image. Their `ENGINE_TODO.md` holds those.

- **The docs taught a `@compileFor` spelling that silently deleted code — FIXED 2026-09-01.** Kept as a
  record because it is the house rule's own failure mode, caught by a user rather than by us.
  [SPEC.md](SPEC.md) and [KEYWORDS.md](KEYWORDS.md) both taught `@compileFor(NATIVE)` / `(WASM)` /
  `(EMBEDDED)` / `(WINDOWS)` — **four sites, one more than was reported** — and not one of those is a
  flag: a built-in target NAME deliberately does not become one, because gating on it would gate on how
  the build was *spelled*. A manifest build rejects the name; a **loose** build reads no manifest and
  treats an undeclared flag as inactive, so the documented spelling compiles clean and the declaration
  is **gone**, with the only symptom a missing symbol somewhere else — or nothing at all if both sides
  were gated. ⚠️ **`tools/check-compilefor.sh` §4b already proved the COMPILER rejects such a name,
  which is precisely why the docs drifting was invisible**; the guard now greps the docs too (§7, and
  it was arming-tested against an unfixed doc before being believed). Same commit dropped
  `a.byteLen()`, documented twice beside `dataPtr()` and — verified across every commit in the repo —
  **never implemented at all**; write `cast<usize>(a.length()) * sizeof(T)`.

- **The audio-seam cluster — found by the first external project, 2026-09-01.** Ten gaps hit designing
  one audio device seam, recorded together because they are one story and because every citation was
  re-read against this tree before it was filed. The stake in their words: their engine says *"the
  platform split is one mechanism throughout — a `type contract` with `@compileFor`-gated
  implementations"*, and **that is not true today and cannot be made true** — the first two below are
  exactly why, and their shipped platform seam had to push its split down into C `#ifdef`s instead.
  ⚠️ **Every claim here was verified against the compiler, and two of their three "small wins" are
  small while the third is not** — sizing a user's report is our job, not theirs.

  - **THE PLATFORM SEAM (`@compileFor` on `extern`; attributes on members; `InlineArray` bridges) — SHIPPED `0.9.131`.** `@compileFor` gates `extern "<h>";`, `extern fn` and `fnptr`;
    `@noheap` marks a method, `ctor`, destructor or operator; `InlineArray<T>#(N)` has `dataPtr()` and
    `view()`. Record in [SPEC.md](SPEC.md) (*Conditional compilation*, *No-heap subset*, the container
    section). Three findings worth keeping, none of them in the row as written:
    - ⚠️ **The row was a GRAMMAR-SHAPE problem, not eleven missing features.** `attribute_list` had
      exactly two arms — a twin of the `fn … block` free function and a twin of `field_declaration` —
      so every other form simply had no attributed twin. Hoisting the prefix to one arm each
      (`attribute_list plain_function_declaration`, `attribute_list plain_class_member`) gave all
      eleven forms attributes at once and DELETED both duplicated twins. Bison still reports exactly
      the one dangling-`else` conflict `%expect 1` accounts for.
    - ⚠️ **The `InlineArray` bridges were mis-sized here as "S … a plain consistency hole rather than a design question".**
      `dataPtr()` was one line; `view()` was not. It needed the `View<T>` instance force-registered, C
      emitted from the emitter (a runtime macro cannot name the program-specific `View_<T>`), the
      `Viewable<View<T>>` grant on the synthetic ClassInfo, and — the part no reading predicted — a
      PASS rather than a line in `registerFixed`, because whether `std::collections::View` existed yet
      depended on the user's unrelated imports. It also exposed `mintReturnTypeNode` having no route to
      an intrinsic receiver, so a `borrow` alias over an `InlineArray` came out untyped.
    - ⚠️ **The trap that would have made the `extern` gate a silent no-op:** `CEmitter::emit` (the single-TU
      `transpile` path) ran `emitIncludes` BEFORE `collectProgram`, i.e. before `pruneInactiveDecls`,
      while `emitProgram` had the order right. Reproduced before fixing; `check-compilefor.sh` now
      asserts it on that same path, and the guard was confirmed to FAIL against the unfixed compiler.

  - **`@noheap` IS TRANSITIVE — SHIPPED `0.9.132`.** A `@noheap` body may not call anything that
    allocates, at any depth; the diagnostic names the chain. The rule is one sentence: **infer where the
    compiler can see the callee, require a declaration where it cannot.** So an ordinary un-annotated
    helper is fine exactly when it is allocation-free (nothing in `lib/std` needed marking, and nothing
    was marked), while a `fnptr`, a bound function pointer, a contract member and a `virtual` slot are
    blind seams that must carry `@noheap` on the DECLARATION — checked against every implementation, the
    way `const fn` already is. Record in [SPEC.md](SPEC.md) *No-heap subset*. Four findings worth keeping:
    - ⚠️ **The detector is the gate, not a walker.** Allocation facts are recorded by `rejectIfNoHeap`
      itself and call edges by `emitReorderedCall` — the one function every resolved call funnels
      through — so detection cannot drift from the gate, because it IS the gate. A separate AST pass was
      the obvious design and is wrong twice: two of the seven allocation sites (boxing a primitive / an
      error into an owning contract handle) are TYPE-directed and invisible to any walker over source,
      and a second walker obliged to know every allocating node kind is exactly how `scanExprForGenerics`
      drifted by three node kinds and began failing open.
    - ⚠️ **The leaf decides everything, and `GlobalAllocator` is it.** Every chain ends at an `extern fn`,
      so propagation alone would have proven only "reaches no `new`" — and a container does not allocate
      with `new`, it goes through its `A: Allocator`. Naming that one leaf is what makes `list.add(x)` in
      an audio callback an error. It needs no annotation to stay precise: `A` is a type parameter, so
      `DynamicArray<T, BumpAllocator>` is a different monomorph reaching a different `allocate`, and the
      arena idiom real-time code actually uses stays legal for free. `deallocate` counts too — `free` can
      block on the allocator lock exactly as `malloc` can, which is what makes merely OWNING a container
      in the region a defect.
    - ⚠️ **The destructor edge is recorded from OWNERSHIP, not from the call.** RAII is what runs at the
      end of a real-time scope, and a `T__dtor(&x)` is emitted from ~20 places (scope cleanup, condition
      temps, assignment drops, match subjects, unwind paths). An edge duplicated across twenty sites fails
      open the moment one is missed, so the edge is taken in `recordDestructibleLocal` instead: declaring
      a destructible local IS the fact, and where the emitter chooses to run the destructor is a lowering
      detail the proof does not model. `emitDtorDefinition` also never set `_currentFunc` — harmless for
      the friend-accessor match it was written for, a silent mis-attribution for anything keyed on it.
    - ⚠️ **An intrinsic is invisible to the walk, and `string` is the one that mints.** A `string` method
      has no AST body, so it contributes neither an edge nor a fact — `"${a}${b}"` was rejected in a
      no-heap region while `a + b` was accepted, the same allocation with two answers. Derived from the
      RETURN TYPE (an intrinsic string method returning a new owned value allocates) rather than a list of
      names, so a method added later is covered the day it is added. ⚠️ Scoped to `String` deliberately:
      the smart-pointer intrinsics also return an owning value — `Shared.downgrade` hands back a `Weak<T>`
      — and allocate NOTHING, so a bare `ownsByValue(return)` rule would have rejected them.
    - ⚠️ **`T__copy(&(x))` was spelled at TWELVE sites; it is one `copyCall` now.** A deep copy of an owning
      type allocates by definition, and `foreach (string s in xs)` allocates once per element with nothing
      in the body that looks like an allocation (the iterator deep-copies to yield by value; `foreach (ref
      string …)` borrows and is legal). ⚠️ The refactor was proved codegen-neutral by diffing the emitted
      `.c` of every fixture across it — 680 files, zero differences — rather than by reading it.
    - ⚠️ **A prelude body can fail a `--no-heap` build the author never wrote.** Gating the string copy
      program-wide broke a program that sorts three integers, because `Template.part`/`hole` in the PRELUDE
      do `return copy this._parts[at]` and every prelude body is emitted. The stdlib's escape for exactly
      this (`@compileFor(!NOHEAP)`, as on `sort`) **cannot be spelled on a member**. So the copy fact is
      recorded for the analysis but rejected only under the ATTRIBUTE — a second concrete instance of why
      the flag half below is not a one-line change.
    - ⚠️ **A dead special case, caught by writing the fixture.** Smart-pointer drops were given a
      hand-written fact on the assumption their destructors are runtime C. They are not: `Owned<T>__dtor`
      is EMITTED and calls `GlobalAllocator.deallocate`, so the ordinary edge already reached the leaf and
      gave the better diagnostic. The real bug was elsewhere — a by-value smart-ptr PARAMETER is pushed
      onto the root scope through `_pendingParamDtors` and never reached `recordDestructibleLocal`, so
      `@noheap fn consume(Owned<Node> o)` compiled while a `DynamicArray` local one line away did not.
    - ⚠️ **The claim that motivated the whole row was invisible to the claim guard.**
      `check-doc-claims.sh` matched "IS a compile error" and the SPEC sentence said "MAKE every … a
      compile error", so the strongest promise in the section carried no fixture and went unpinned long
      enough to become false. The pattern is widened and the claim is marked. A claim regex that
      recognises one grammatical voice has a blind spot the size of the other.

    **THE FLAG HALF SHIPPED `0.9.147`, and the design question it was parked on had a wrong premise.**
    "A user-code/library distinction the emitter does not have" was true of `diagFile()`, which is what
    had been looked at, and false of the emitter: `declFile` is recorded at COLLECT time and carries the
    `<`-sentinel `checkReach` already reads. ⚠️ **Neither half of the test works alone, and each covers
    exactly the other's blind spot** — the sentinel says nothing about `lib/std`, which is parsed off disk
    under its real path, while the module-path test (collision-proof, since `std` and `core` are reserved
    segments) calls the PRELUDE user code, because the prelude has an empty namespace scope and
    `GlobalAllocator::allocate` renders bare. ⚠️ And `declFileOf` answers "" for a mangled
    `Class__method`, so a member must be judged by its OWNER's file or every method seeds nothing.
    The diagnostic anchors on the innermost user body — the frame holding the call the author can change —
    which also kept the message count at one per defect instead of one per stack frame.

  - **SHIPPED `0.9.160`/`0.9.161` — no `-fsanitize` flag remains, on any target or tier.** The consumer's
    finding was that kama's unconditional `-fsanitize=integer-divide-by-zero,shift-exponent,
    float-cast-overflow` (with `-fsanitize-trap`, so no sanitizer runtime) made emscripten refuse
    `-sWASM_WORKERS`, and with it AudioWorklet — no audio thread in the browser at all — and that the
    refusal was over-broad for trap-only mode. The answer taken was the second of the three they offered,
    on every target rather than wasm alone: the four faults are now the compiler's own checks in the
    emitted C (`KAMA_DIV`/`MOD`/`SHL`/`SHR`, `kama_f2i_chk`, and in debug `KAMA_ADD`/`SUB`/`MUL`/`NEG`
    plus the place operators for `+=`/`++`), which cost the branch the sanitizer already cost, print a
    message where `ud2` printed nothing, run the panic hook, and are what an `@onPanic` region can recover
    from. Release codegen parity is asserted by `tools/check-release-arith.sh`. (Wasm Workers also need
    SharedArrayBuffer and therefore COOP/COEP headers — a hosting constraint, not kama's.)
- **The defects the first external project's queue turned up, and what each one cost to find.** All
  reproduced against the shipped compiler before anything was written; two were fixed in `0.9.148` and
  `0.9.149`, one in `0.9.150`, `0.9.170`–`0.9.175` closed the raw-seam triage (KB-12, KB-14, KB-15 and
  three findings of ours — the git log has each), and the residue below is what is left. Their KB-13
  (triaged 2026-09-04 against `0.9.164`) is the first open bullet.

  - **A module `static` cannot own a destructible resource — considered, deliberately DEFERRED, not a
    row (2026-09-04).** `static World g = World.make();` is refused, and the diagnostic states this stance.
    It surfaced as the ROOT of the first consumer's calloc'd `World` (on the web `main`'s frame is unwound
    while the rAF callback lives), which is what made it look like the fix. It is not, and the reasoning
    is kept here so it is not re-derived. (The seam it was mistaken for — a local `UnsafePtr<T>` element
    is deliberately untyped to ownership so `nd[i] = od[i]` stays a bitwise relocate; widening `exprClass`
    fixes their KB-14 and breaks 45 fixtures — became DIAGNOSTICS in `0.9.174`, and then the audit found the
    narrower fix: a CALL is not a store, so `0.9.227` types the RECEIVER alone through `ptrLocalElemType`
    and `p[0].m()` resolves on a local as it always did on a field, `exprClass` untouched; `drop` through a
    raw element stays refused, the SPEC has *The raw seam*.)
    - **kama's `static` is a fenced MCU tool, not a general global.** Per-isolate (`KAMA_ISOLATE_LOCAL`),
      compile-time initializer only (no init order, no hidden constructor before `main`), unreadable in a
      `@foreignEntry` region unless assigned there, and typed to the MCU shapes — value, `UnsafePtr`,
      `InlineArray`, `Simd`. "Support it in full" means removing fences that exist for the MCU shape (no teardown on bare metal, per-isolate on a host); a host driver object is `Owned<T>.release()` — so this is genuinely optional, not "nobody asked".
    - **The need it was mistaken for is answered at the foreign boundary instead — SHIPPED `0.9.175`.**
      `HeapOwner<T>` had `adopt` (Rust's `Box::from_raw`) and no twin, so ownership could enter kama from
      a foreign API but not leave it. `Owned<T>.release()` lets the foreign API's own `userdata` slot hold
      the lifetime, as every ownership language does at its foreign boundary: hand it over in one
      `unsafe fn`, borrow through it in the callback via a `ref T` parameter, `adopt` it back to destroy.
      That is GOALS §3a/§3e verbatim — persistence is ownership, and the boundary is where ownership
      crosses. ⚠️ **Say "foreign", never "C", on this surface** — the vocabulary is already
      `@foreignEntry`/`extern`, and the rule `@linkName` was named by applies: the host is C today
      and a VM or another backend tomorrow (GOALS §10). The diagnostic, the SPEC section and the method's
      comment all name the concept.
    - **What it would cost if a real case ever pulls it**: a dtor seam at both teardown sites (the
      synthesized `main` after `kama_main`, cemit ~21365, and the isolate trampoline ~5153, since statics
      are per-isolate), and `isConstInitExpr` accepting `Optional::None` so the static starts absent in the
      TYPE (§3b) rather than through a runtime initializer. (The generic-resource reassignment leak that
      assigning such a static would have hit is fixed, `0.9.173`.) A pulling case would be an
      isolate-local driver object with a destructor; on an MCU nothing exits, so even there the gap is
      the initializer. If a case arrives, this entry is the design; reopen it as a row then.
  - **KG-15 in their doc is stale**: `Mat4 * Vec4` is caught by `kama check` today ("the right-hand
    operand expects a `Mat4`, so it cannot be given a `Vec4`"), and check and build agree.

  - **SHIPPED `0.9.176`–`0.9.181` — the two wrong-file rows, and three more defects the first one was
    hiding.** Both ROADMAP rows are deleted; what follows is the record, then the original filing.
    - **The KB-13 row was not a message bug.** The filing (and this section) described a spurious import
      error. It is that, but the same read-site resolution MISSES across a module boundary rather than
      firing: the consumer's scope prefix differs, `_classes.find` comes up empty, and the field is
      silently dropped from the analysis. **Measured, one program written twice**: `slot Box b;` inside
      `Box`'s own module emits the field-default fill `b.c = Counter__start();`; one module over it emits
      nothing and the field keeps its `{0}`. Identical source, different C, decided by the reader's
      imports. ⚠️ It has **no kama-level observable** — the only legal way to fill a `slot` is an `out`
      argument, which overwrites the whole value — so it is pinned by `tools/check-module-emit-parity.sh`,
      which writes one program twice and diffs the emitted C, not by a fixture.
    - **The prescribed fix point was wrong, and the note below still says so.** "Bake at collect time as
      `bakeConstSizes` did" cannot work: `resolveUserNameImpl`'s `known()` predicate requires the target to
      be in `_classes` *at that instant*, so every forward reference would bake to a bare, permanently
      wrong spelling, and generic instances do not exist yet. `bakeFieldCTypes()` is a whole-program pass
      beside `computeDestructible`, which already performs this exact resolution with the declaring class's
      scope installed. It is reach- and xref-silent for a CHECKED reason: `_refUnit` is null throughout
      `collectProgram`, so `checkReach` and `recordRef` both return at their first line.
    - **The `diagFile()` row reproduced far harder than filed.** Not "an 8-line repro blamed at line 69":
      a **four-line** program built with `--strict-numeric` produced **26 rows, every one naming that
      program, not one of them at a line it has**, the highest claiming line 531. Five header-pass blocks
      now install their owner the way `emitStruct` already did. ⚠️ `--strict-numeric` is the only
      observable this class has, and that is why no fixture drove it: the defect needs a position raised
      while a prelude BODY is emitted, and the prelude compiles clean, so no error fixture can reach it
      without breaking the prelude itself. `check-diag-file.sh` case 9.
    - **A position that leaves the compiler now names a real file, and that path is VERIFIED.** The
      prelude and the triad are compiled from text embedded in the binary while `builtinSourcePath`
      resolves a file on disk, and it checked only that the file existed. Measured: insert five lines at
      the top of a copy's `prelude/global.kama` without rebuilding, and go-to-definition on `Optional`
      still answered `global.kama:8`, where line 8 had become a comment and `Optional` had moved to 13 —
      a bug the query layer already had, which the diagnostic consumer would have inherited. The embedded
      text is in the binary, so it is compared against; a mismatch yields "" and every consumer degrades
      to the `<prelude>` sentinel. Go, Zig and C compile their stdlib from disk and cannot have this;
      Rust (`/rustc/<hash>/…`) and the JVM ("source does not match the bytecode") make it detectable
      instead. `tools/check-builtin-path.sh`.
    - **Every class member was reported at the previous construct's line.** Found while trying to assert a
      line for the conformance fix. Each member rule starts with the nullable `modifiers_opt`, so on the
      empty derivation `YYLLOC_DEFAULT`'s `N == 0` branch gave it the end of whatever preceded it — the
      same skew `STAMP_START` was written for at top level and never applied to members. A member without
      `public`/`static` is the common case, not the corner one. Five `DIAGNOSTIC_LINES` rows moved, all
      corrections; the sharpest is `ser_unmarked_field`, which blamed the `@field`-MARKED field for the
      unmarked one. ⚠️ The test is `$1->empty()`, not `!$1`: `modifiers_opt` reduces to an empty list,
      never to null, and written the other way it compiles, runs, changes nothing and reads like a fix.
    - **A conformance defect was reported as the contract's file at the implementer's line.** Rendering a
      contract's vtable slots reseats the emitter onto the contract's file (correctly — its imports are
      what the signatures resolve through) and the six conformance reports rode along. **The rule, now
      written at the site: the file must come from whichever declaration the LINE came from.** That
      pairing is the whole class: `Diagnostic.file` and `srcLine` are assembled from independent sources
      and nothing checks they agree. The three existing fixtures could not see it because contract and
      implementer share a file; `tests/xfail/contract_public_cross_file.d/` is the cross-file one.
    - What the fix unmasked (an unresolved name or member left to clang) shipped in `0.9.182`–`0.9.184`;
      the surface claim is in SPEC "Scope resolution uses `::`", the residue is the enum-initializer row below.

  - **A file must import a type it never names (their KB-13) — the original filing.** Twelve lines, two files in one module:
    `decl.kama` exports `type enum Kind` and a `type value Holder { public int32 n; public Kind k; … }`;
    `main.kama` imports `Holder` and `makeHolder` only and declares `Holder h = makeHolder();`. Result:
    "`Kind` is declared in `src/decl.kama` and this file does not import it", against a file that never
    spells `Kind`. Located with a backtrace, not read: `checkDefiniteAssignment` classifies a local's
    fields as owning or not by calling `cType(f.type)` on each field of the class, `cType` resolves the
    field's type through `resolveUserName` with the FIELD's own identifier node as the site, and
    `checkReach` judges that node — text from the declaring file — against `refFilePath()`, the consumer
    being emitted. Measured shapes: a `type value` and a `type resource` holder both fire, an enum field
    and a `type value` field both fire, a resource holder fires **three** times (so at least two more
    read sites of the same shape exist); a discarded `makeHolder();` and an inline `makeHolder().n` do
    not, and a primitive-only holder does not. ⚠️ **This is the fixed `InlineArray`-size bug's family
    exactly** — a member's declared type resolved wherever it is READ — and that bug's record above says
    which fix is right: bake the answer at collect time, where the declaring file's scope is installed;
    the read-site scope swap is the measured wrong one (30 fixtures, 17 agreement pairs). "Is this field
    owning" is a fact about the class, not about the reader. **The position is a second wrong-file
    mechanism**: `site->line` is the field's line in `decl.kama` (7 in their repro, 5 with a value field)
    stamped with `main.kama`'s path — a 6-line file blamed at `7:0`. `run_tests.sh`'s
    `diag_position_faults` would flag it, in a `.d/` fixture that exercises it. Their cost statement is
    the one to keep: adding a typed field to a widely-held `type value` is a breaking change to every
    file that merely holds one, invisible from the type's own definition.

  - **KB-13's RESIDUAL — a field PASSED ALONG still demanded the import — FIXED `0.9.189`.** Re-audited
    2026-09-05 against `0.9.188`: `f489d19` closed the declaration (`Holder h = makeHolder();` built with
    `Kind` unimported), and the one shape left was handing the field to a function that declares the type
    itself — `takesKind(k: h.k)` in a file that imports `Holder`, `makeHolder` and `takesKind` and never
    spells `Kind`. Same family, same rule: `typeOfExpr` took the first non-empty of four resolvers and
    three of them re-resolved the field's type NODE under the reader's scope; every field-type read now
    goes through `fieldCType()`, the bake. Across a MODULE boundary that resolution used to MISS rather
    than fire, and what went quiet with it was the enum-identity rule — a `Kind` handed to an `Other`
    parameter was accepted; it is refused again. ⚠️ The measured wrong fix still applies — do not install
    the owner's scope at the read site (the five-site `NsCtx` partial-swap hazard).

  - **A diagnostic can name the USER's file at a line that does not exist in it — the original filing.** `diagFile()` prefers
    `_collectingUnitPath`, then `_emitDeclFile`, then the file being compiled — and for a prelude or
    stdlib body emitted in the HEADER pass the first two are empty, so the error is stamped with the
    user's path and the library's line number. An 8-line repro was blamed at line 69. ⚠️ **It has been
    worked around twice already rather than fixed**: the `GlobalAllocator` leaf stores no position at all
    (`AllocSite{…, 0, ""}`) and `CEmitter::line()` is deliberately NARROWER than `diagFile()` for the same
    reason, with a comment saying it must not be widened to it. `run_tests.sh`'s `diag_position_faults`
    would catch the class, but only where a fixture drives it, and none does.

  - **No way to ask for reproducible floating point — SHIPPED `0.9.169`** as the manifest key
    `reproducible-float`. Measured here rather than taken from the report: **13** of 64 random `a*b + c`
    triples differ on aarch64-macos, 0 with `-ffp-contract=off` — and ONLY in a release build, which is
    the finding worth keeping (see the vacuous-fixture note above).

  - **An `InlineArray` field's SIZE had to be imported with the type — FIXED `0.9.150` (their KB-11).**
    Indexing `b.cells[0]` across a package boundary needed `geom::N` in the reader's import block; the
    same shape inside one package always compiled. ⚠️ **The diagnostic named the wrong problem
    entirely** — "raw pointer access requires an `unsafe fn`" for a missing-import bug, pointing the
    reader at `unsafe`. The mechanism is a SILENT SKIP: a member's declared type is resolved wherever it
    is READ, and `registerFixed` returns quietly when the size will not fold, so the field stopped being
    a container. The LAYOUT was never wrong — a consumer calling a method that indexes the field
    internally built and returned the right value. Fixed by BAKING the size at collect time, where the
    declaring file's scope is still installed. ⚠️ **The read-site fix was tried first and is wrong:**
    installing the owner's scope in `cTypeInInstance` for a non-generic class broke **30 fixtures and 17
    analysis-agreement pairs** — the five-site NsCtx partial-swap hazard, and the same seam the
    class-identity rule's generic-body gate sat on until `0.9.185`.

  **⚠️ What their queue is worth reading for.** KG-15 was filed as "ergonomic friction" and was a
  check/build divergence reaching four positions, not one; KB-10 was filed as an `InlineArray` problem and
  was `isConcreteTypeArg` not recognising a raw pointer, which broke EVERY generic inference over a
  pointer element; KB-11 was filed as an import problem and was a silent skip plus a size resolved
  in the wrong scope; KB-13 was filed as an import-rule problem and is the KB-11 read-site resolution
  again, at three or more sites. **All four were bigger than the report, in the same direction: a
  consumer describes the shape they hit, not the rule that is wrong.** Re-derive the rule before sizing
  the fix. KB-12 is the exception that proves it from the other side: filed as "emit a terminator after
  the match", it is the statement-form half of a fix that already exists, and the right change is the
  sibling's, not the one suggested.

  - **This bucket is EMPTY**, and what emptied it is worth keeping, because both items were parked on a
    dependency rather than on a judgement. The bucket said they waited on `@noheap` transitivity and the
    foreign-thread entry; transitivity shipped, which discharged half of that immediately.
    **`@noheap` on a `fnptr` type** then went from nice-to-have to needed by the very campaign that
    unblocked it — a `fnptr` is a blind seam the proof cannot cross, so the call became a HARD ERROR in a
    no-heap region with nothing an author could write to permit it — and it **SHIPPED `0.9.146`**:
    `@noheap` on the signature, every bind checked against it, the call then provable
    ([SPEC.md](SPEC.md) *No-heap subset*). ⚠️ **It was not the grammar change everyone assumed**, which is
    the reusable part: `attribute_list plain_function_declaration` already covered the `fnptr` arm and
    tree-sitter already had `optional(attribute_list)` on it, so the attribute PARSED and was rejected
    semantically — no bison edit, no regenerated `grammar.bnf`, no Zed pin. ⚠️ **And it exposed that the
    bind was checked in ONE position out of six:** `emitFnPtrBind` is the only caller of `sigMatches` and
    a local declaration is its only caller, so an assignment, an argument, a return, a field and a module
    `static` bound through `emitExpression`'s bare-name arm, which checked nothing — a shape-mismatched
    function bound there compiled, and the call passed the wrong argument count. **A per-region panic
    policy** was promoted by measurement rather than by argument — see the consumer's underrun numbers
    above — and **shipped in `0.9.162` as `@onPanic(recover: <literal>)`** (SPEC *Recoverable regions*):
    `setjmp`/`longjmp`, gated on `@noheap` plus a transitive no-destructible-local walk, the hook not
    run for a recovered panic. ⚠️ `__builtin_setjmp` is not supported on arm64 macOS, so `<setjmp.h>` is
    included only by a program that declares a region. ⚠️ **A "not scheduled" bucket whose reason is a
    dependency needs re-reading every time that dependency ships**, or it silently becomes a list of
    things nobody will look at again.

- **A METHOD and a CTOR cannot take type or `comptime` parameters** — only a free function can. The
  `type_params_opt` slot appears in exactly three grammar rules ([kama.y](../src/kama.y), the
  `function_declaration` arms); `method_declaration`'s four `FN` arms and both `CTOR` arms have none, so
  `fn T widen<T>()` inside a type is a *parse error*, not a diagnosed restriction. A generic **type**'s
  members are unaffected — they monomorphize per the enclosing type's parameters, which is the common case.
  What is out of reach is a member introducing a parameter the receiver does not have.

  **The idiom is a free function with bounds**, and the stdlib uses it throughout:
  `fn void sortWith<T, C: Order<T>>(View<T> items, ref C by)`
  ([lib/std/collections/sort.kama](../lib/std/collections/sort.kama)) is a free function precisely because
  `C` is a second parameter `View<T>` cannot introduce. The method form *is* expressible — take
  `ref Order<T> by`, a contract borrow — at the cost of dynamic dispatch where the free function
  monomorphizes to a direct inlinable call. So this costs **ergonomics, not capability or performance**,
  which is the same profile as the `fnptr` entry above and the same reason it waits. Note also that the
  turbofish's absence on a method is *not* an extra restriction: `3c9b441` removed the one receiver
  turbofish (`r.deserialize::<T>()`, sugar for a `__kamaDeserialize<T>` free trampoline), and with no
  generic methods a method turbofish has nothing to name. Verdict (audit, 2026-09-07): a **non-goal** — a method's own type parameter would need a second turbofish grammar on a receiver call, and the free-function spelling above is the idiom.

- **A `comptime` parameter's type is an integer, `bool` or `char`** ([kama.y](../src/kama.y),
  `comptime_param_type`) — no compile-time float, array or struct parameter. This is where a
  user-writable "this argument must be compile-time constant" would come from. Compile-time values now
  live in their own list — `comptime(int32 N)` at a definition, `#(4)` at a use — so widening the set
  further is purely additive; see [SPEC.md](SPEC.md) *Generics*.
  ⚠️ **Rust has shipped comptime parameters since 2021 and still restricts them to integers, `bool` and `char`**,
  because a composite value in a parameter list has to be encoded into a mangled symbol name. That is the
  constraint, not an oversight to fix. The compiler can still *require* a constant argument for its own
  intrinsics — `Simd`'s `shuffle(pattern:)` does — the same by-name knowledge it has of `InlineArray`'s
  `get`/`set`/`length`.

- **A non-constant turbofish argument reports the wrong thing.** `shifted(x: 2)#(runtime)`, where
  `shifted` *is* a generic function, says *"turbofish type arguments are only valid on a generic function"*
  — which is false and points away from the real problem, that `runtime` is not a compile-time constant.
  A bad diagnostic rather than a hazard (the program does not build), but "the error names the wrong cause"
  is exactly what kama's diagnostics exist to prevent. Wants an `xfail` fixture in the same commit as the fix.

- ~~**A `foreach`/`match` binder named like a FIELD.**~~ **ANSWERED and SHIPPED `0.9.253` — kama has no
  shadowing, and the ban now reaches every binder.** The row asked whether to extend the field check to
  binders; the right question was why binders were outside the ban at all, and the answer is that they
  never were by decision. The check simply lived in `emitDeclarator` and nothing else called it. Its
  absence was rationalized once in a source comment (*"the consumer-driven audit answered it: the
  exemption STAYS"*) that no row, SPEC sentence or fixture ever backed, and `0.9.248` had made the
  shadow WORK while fixing consumer KB-20 — a language decision taken inside a bug fix, in the wrong
  direction. `checkBinderShadow` now refuses a `for` counter, a `foreach` variable and a `match` payload
  binding that takes the name of a parameter, an enclosing-scope local or an in-scope field, with the
  same three messages the declarator gives. Sibling-scope reuse is untouched (`0.9.247`), which is the
  half of KB-20 that was a real bug. Six `tests/xfail/binder_shadow_*` fixtures, one per binder × message.
  ⚠️ **The corpus had exactly one instance and it is the argument for the rule**: `tests/net_addr_ctor`
  wrote `case V4(a: a, b: b, …)` while two `SocketAddr` locals named `a` and `b` were live — and the
  match SUBJECT was the outer `a`. It read as destructuring `a` into itself.

<a id="s3"></a>

## 3. Open design questions (settle before the work they gate)

- **Modular / opt-in stdlib — does "pay for what you use" pruning scale?** The **prelude mechanism**
  (`PRELUDE_SRC`) is the seed: a stdlib = more prelude-collected kama modules in a `Std` namespace. Generic
  types emit only when instantiated, and `--gc-sections` prunes unused functions in release. Open: whether that
  pruning suffices, or explicit per-module opt-in / dead-function elimination is warranted before a large stdlib
  grows. (`std::math`/`std::io` already ship as directory modules under this mechanism — the open question is
  whether pruning scales, not whether the packaging shape works.)
*(The `Slot<T>`/`MaybeUninit` spike that sat here is answered and shipped: the shape is a `slot`
DECLARATION, not a wrapper type — no new type, no `.assume_init()`, and an unassigned slot simply has no
drop emitted. See SPEC § *Uninitialized storage*.)*

<a id="s4"></a>

## 4. Reflection + serialization — remaining follow-ups (1.x)

### `Handle` decodes malformed input as `Ok` (KR-51) — found 2026-09-12

`Handle.deserialize` (`lib/std/collections/slot_map.kama:28`) reads its two fields and returns `Ok` without
asking the reader whether a read failed, so `deserializeJsonBuffer::<Handle>` on a malformed document hands
back a garbage handle as success — and the JSON/KBIN entry points do not consult the sticky flag after an
`Ok`, so nothing downstream catches it. The fix is the one every other `deserialize` already makes: check
`failed()` and return `Err(errorCode())`. **Not a boundary-level safety net** that converts an `Ok` from a
failed reader: a type that fails returns `Err`, and nothing papers over one that does not. It waits for
reach-based `--no-heap` (KR-47): the new error box would otherwise fail every `--no-heap` build that merely
imports `SlotMap`, which `0.9.295` relaxed on purpose.

### The architecture review, and what it settled — SHIPPED `0.9.270`–`0.9.274`

The 2026-09-09 review judged the shipped serde layering over-engineered and blocked the container work on
an architecture decision. That decision is now made and executed. The four verdicts, and the evidence:

| commit | what it did | verdict |
|---|---|---|
| `43dd3dd` **mechanism** | `Owned<T> implements Serializable` in the triad's own file | **REVERTED.** Handles auto-deref, so a domain method on one shadows the pointee's — and it was already biting: `this.o.serialize(w)` bound `Owned_T__serialize` while `this.s.serialize(w)` bound `T__serialize(Shared_T__deref(…))`. The outputs agreed only because `Owned`'s serialize happened to write the pointee inline |
| `43dd3dd` **outcome** | an `Owned` subtree written inline; reach walks *through* `Owned` | **KEPT**, re-implemented as recognition by identity (`ownedPointeeOf`). Bonus the maintainer named: `Owned<T>` and bare `T` produce identical bytes, so a field may change between them and still read the other's data |
| `fab2c89` **decision A** | the envelope became ordinary tokens | **KEPT** — and it *is* the maintainer's original design: references map into an ARRAY of entries with first-come-first-serve ids. The pre-`fab2c89` code framed that table as an id-KEYED OBJECT, which is the implementation detail that diverged, and exactly why a positional backend could never carry a graph (`beginObject`'s count is compile-time shape and never rides the wire; `beginArray`'s length does) |
| `fab2c89` **decision B** | the algorithm moved to the library | **REVERTED.** It alone produced `ObjectGraph<T>`, 5 prelude contracts and 7 public synthesized members per node type. It also made the COMPILER bigger: `fab2c89` was +503/−317 in `kama.cemit.cpp` |

**What a node type is now:** the same two members a by-value type has. No wrapper — what you hand a backend
IS the root — and only the READ changes shape, giving back `Shared<T>`, because a cycle cannot be returned
by value. The walk is four internal C functions plus a pure-C id table in `kama_runtime.h`; that substrate
is load-bearing rather than incidental (identity dedup and a heterogeneous node list are the two things
tokens cannot express, and it cannot be a kama type because a collection is itself serializable).

**The spelling for "T is Serializable OR a pointer" already existed: `T: Serializable`.** All three handles
satisfy the serde bounds through a DERIVED arm in `satisfiesBound`/`classSatisfiesBound`, beside
`Immutable`, `Sendable` and `Copyable` — the same shape, a property the compiler knows by nature with no
declaration site, made nameable in a bound. It stays derived rather than a method on the handle for the
reason the triad is method-free at all; a bound has no shadowing hazard, because nothing calls it by name.

⚠️ **A bound cannot answer a graph question.** Measured twice: bounds are judged while a generic instance is
registered, DURING collection, and every graph fact (`reachesPointer`, `graphDeserialize`) is a
whole-program result settled afterwards. Requiring one at the bound made every real graph fixture fail.
This is why two refusals land at the lowering and report against stdlib source — see the attribution row.

**Three `kama check`-green / clang-red holes surfaced, two closed:** a node reached only inline kept a
by-value `deserialize` whose `Shared<Leaf>` field called a symbol nothing defines; `deserializeJsonBuffer::<Node>`
for a graph node type-checked and mismatched in C (this restores, in new terms, the guard
`graph_root_unwrapped` used to give). The third — boxing a `string` into a contract — has its own row.

**The container seam is NOT what the review predicted.** §4 said "no language ruling gates the walk" on the
strength of `xs[0].m()` compiling. That is true concretely and false where it matters: `this[i].serialize(w)`
does not resolve for a GENERIC element, which is why `dynamic_array.kama` hops through a `const ref T`
helper — verified by deleting the helper and watching it fail. And a `ref` parameter may not name a smart
pointer. One language rule therefore gates every container-of-pointer shape, `Owned` and `Shared` alike.

### The graph channel — SHIPPED `0.9.276`–`0.9.278`

The maintainer ruled the container row GENERAL on 2026-09-10: **one rule for every `Serializable`** (any type
reaching a `Shared`/`Weak` takes part, hand-written or derived alike — no second-class `Serializable`), and
**any serde type may be a root** ("the reader must know what they are trying to read regardless"). What
`@generate` buys is a FIELD WALK, nothing else; the walker existing only as a synthesized body was the
accident, not the design.

⚠️ **This was not a missing feature, it was a live correctness bug.** Measured at `0.9.275`: a hand-written
`serialize` over a type holding two `Shared<Leaf>` handles to ONE leaf compiled clean and wrote
`{"a":{"v":9},"b":{"v":9}}` — the pointee inline, twice, identity gone with no diagnostic — and the same
body over a cycle was a **SIGSEGV**. It now writes `{"root":{"a":1,"b":1},"objects":[{"id":1,…}]}`, and a
hand-written cycle is a compile error naming the pointee that must be `@generate`.

**The mechanism is a PARAMETER, not ambient state.** A `KAMA_ISOLATE_LOCAL` "current graph" was proposed and
rejected: the graph is ALREADY a per-invocation stack local in the driver, and every call below it is a
direct monomorphized call whose signature the emitter owns. A participant's body is emitted once under a
second name that carries it (`X__serializeInto(self, w, g)` / `X__deserializeFrom(r, g)`), with
`X__serialize` / `X__deserialize` synthesized as root drivers over it.

⚠️ **Never widen `X__serialize`.** The `Serializable` vtbl slot is filled by CASTING `&X__serialize` to the
slot's function-pointer type, so a changed arity compiles silently and then reads an argument nobody pushed
— the one failure mode here that clang cannot catch. The twin is a separate symbol for exactly that reason.
(`Deserializable.deserialize` is declared `ctor` and the vtbl builder skips ctors, so there is no read slot
anywhere and the read side is always statically resolved.)

**Identity decides the `root` slot**, and that is the whole of the root ruling: a node can be pointed at, so
the root carries an **id** and reads back as `Shared<T>`; a collection or a hand-written holder cannot be
pointed at, so the root carries its **value** inline and reads back **by value** — there is no cycle through
it to close. Existing node wires are unchanged.

Things the build corrected, each measured rather than reasoned:
- The edge retarget must fire BEFORE the auto-deref fallback. The triad is method-free, so `serialize` is
  not on the handle, and resolving it on the pointee is exactly the inline write this removes.
- The node closure walks edge FIELDS; a container's edges are type ARGUMENTS. Without recursing through
  them the owner was a node, its field was walked, and every element was still written inline.
- A participant reached as a ROOT is reached from nowhere, so its edges are seeded by a pass of their own —
  **without** marking it `isGraphNode`, which would make `typeHasGraphAdapters` answer true for a container.
- Pass 2 must walk a participant's FIELDS as well as iterate its ELEMENTS. Doing only the latter left a
  hand-written holder's own edges unwired: correct write, stashed ids in the fields, SIGSEGV on first deref.
- "Hand-written" is exactly "has an AST body" — not the synth flags. The `Shared<X>.deserialize` forward
  `computeGraphNodeTypes` installs is neither, so asking the flags gave the TRIAD a twin.
- ⚠️ `DynamicArray<Shared<X>>` had never had a read half for ANY `X`, graph or not: the
  `Result<Shared<X>, Owned<Error>>` its element read binds is minted only by `computeGraphNodeTypes`, which
  runs AFTER the destructibility fixpoint, so the monomorph existed unjudged and the container's own
  `match (give __er)` was refused as "a plain value, copied on assignment". The `Owned` box read hit the
  identical trap at `0.9.276`. Both are minted pre-fixpoint now.

**Discovery runs the body against an emitted discard sink.** A derive's edges can be walked field by field
and a hand-written body's cannot, so it is RUN, with every token thrown away; the edges intern on the way
through, which is what keeps the table's length exact before `beginArray` — the positional and numbered
backends have nothing else to bound a read with. One body therefore serves both passes with no mode flag.

**Reading back needs mutable iteration**, because a container's elements are not fields: `IterableMut<T>`,
the protocol `foreach (ref T x in c)` already requires, so a third-party container gets this by implementing
what it would implement anyway. ⚠️ Pass 2 must NOT instead record each element's address during pass 1 —
`DynamicArray.deserialize` grows via `add`, so the buffer reallocs mid-build and every recorded address
dangles; the stash survives because it rides IN the element and moves with it.

Fixtures: `ser_coll_of_owned` (the `Owned` element and root), `ser_graph_coll_write` (both write shapes,
byte-for-byte), `ser_graph_coll` (round trip on json AND positional — **the check is the re-encode**:
`kids` is `[2,3,2]`, and a read that rebuilt two copies instead of sharing one node would come back
`[2,3,4]`), `ser_graph_handwritten` (the hand-written holder, round trip), `ser_graph_handwritten_node` (a
hand-written NODE with a `Weak` back-edge — a cycle, round-tripped and leak-free).

**A hand-written type as a NODE shipped too (`0.9.279`)**, which is what makes the rule general rather than
nearly-general: its four walk helpers are its twin, and the one thing that could NOT follow the derive is
the root spelling — a node's `deserialize` must hand back `Result<Shared<This>, …>`, which
`computeGraphNodeTypes` rewrites in place for a derive, and there is no rewriting a body whose `return`
constructs a `This` (that by-value body is exactly what fills a SHELL). So the author's signature is left
alone and the handle root is a synthesized second entry point. ⚠️ A predicate that decides whether to emit a
walk must stay the MIRROR of what that walk acts on: `graphPartIsEdge` did not unwrap `Optional` while
`emitGraphFieldWire` does, and an `Optional<Shared<X>>` field went unwired. ⚠️ And the SANITIZER caught what
the compiler could not: a cycle of two STRONG handles leaks by construction, so the fixture wants the `Weak`
back-edge — the leak was the fixture's, not the emitter's.

**Nothing is left of this arc.** The `copy`-marker defect that made `Map<K, Shared<V>>` uncompilable shipped
in `0.9.283`–`0.9.285`, and the two containers still short of a graph closed in `0.9.293`/`0.9.296`.
`Set`/`SortedSet` of edges is refused BY BOUND — the triad is not `Hashable`/`Equatable` — so edges only
ever arise in the sequence containers and `Map`/`SortedMap`/`SlotMap` values.


Serialization ships today (by-value + object-graph + polymorphic contracts) with **two backends — `json` (text)
and `binary` (KBIN)** — see [SPEC.md](SPEC.md) "Serialization". What remains is additive library + hardening:

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T>#(N)` read; a bare `serializeJsonBuffer`/`deserializeJsonBuffer` of an
  intrinsic value. (A `const` field is a separate general language gap — doesn't parse today.)
  ✅ The enum half of this bullet SHIPPED with the derives: a bare `serializeJsonBuffer`/`deserializeJsonBuffer` of an enum value works
  (consumer KB-18 — it was the missing `<Enum>__as_Serializable` vtbl, not a missing wire form), and so do
  generic enums, per instantiation. See *Derive follow-ons* in §2.
- **Binary backend follow-on.** Delta/snapshot replication stays ENGINE-level (above serde); generic byte
  compression is an io-adapter layer (§1 transform adapters), not a serde concern. The schema-locked
  positional mode is no longer deferred: it is the *Positional binary backend* row, built on the design below.
- **Field addressing — SHIPPED `0.9.257`–`0.9.262`.** Where it came from: consumer KG-34 measured a six-field
  frame at 24 bytes of data and 91 on the wire under KBIN, because every object carries every field NAME. A
  per-stream name-interning scheme (the `encoding/gob` shape) was BUILT, measured at 226 bytes where 338 was,
  and **reverted before commit** — kama's serialization sits behind contracts precisely so there can be several
  serializers, and a size knob on the self-describing one hard-codes one consumer's problem into the stdlib
  while making KBIN stateful. ⚠️ **Do not re-propose interning.** The size need is answered by the positional
  back end, which is a different ADDRESSING rather than a cheaper spelling of names.
  What the language surface now is lives in [SPEC.md](SPEC.md) § *Serialization*; the reasoning is in the git
  log. Five things worth keeping here because they were learned by BUILDING, against a design that said
  otherwise:
  - **A generic `Serializable<K>` cannot exist** (measured): a type conforming to `Emit<string>` and
    `Emit<int32>` at once needs two `emit` bodies differing only in a parameter type, and kama refuses that as
    overloading. That is why the key carries every form and the backend keeps the one it is — not a query.
  - **`FieldKey` needed a third case, `Position(rank)`.** The design said a positional reader answers
    `Id(counter++)`. It cannot: the derive looks an `Id` up by VALUE and a positional reader never sees one, so
    `@field(id: 30/20/10)` reported 0,1,2 against a switch labelled 10,20,30 and every field missed. Rank and
    id value also cannot share a switch — with ids `(5, 1)` the label 1 means slot 0 as an id and slot 1 as a
    rank. One case per addressing.
  - **`variant(name, index)` had to join the contract.** The derive spelled a tagged enum's discriminant
    `writeString(variantName)`, so no addressing change could reach it — the name rode through as the *payload*
    of the `"tag"` field. The value vocabulary had one member per scalar shape and none for a discriminant.
  - **`writeSome()` and the reader's `endObject()` likewise.** An `Optional`'s `Some` arm carried no presence
    marker (a self-describing reader recognises presence by the bytes; a positional one has nothing to look
    at), and `moreFields` alone cannot tell a counting reader when to pop a frame, because the derive calls it
    several times on one frame and ignores the result. Without `endObject` a positional reader is not
    implementable at all.
  - **`beginObject(count)` is asymmetric with `beginArray(count)` on purpose:** an array's length is RUNTIME
    data so it goes on the wire and the reader reads it back; an object's field count is COMPILE-TIME shape, so
    the reader is told instead. A corollary that has to be stated: a positional backend cannot carry a GRAPH,
    because a table's size and an entry's field count are runtime facts no positional writer states either.
- **`@bits(n)` per-field bit-packing — NON-GOAL, and now deleted** (it went with the field-addressing row).
  It was never implemented: its whole body set the "field is marked" flag and it never read its arguments, so
  `@bits(4)` and `@bits(banana)` were equally accepted, it appeared in no SPEC text, and it was used in zero
  files — while silently standing in for `@field`. **What answers the need instead:** a packing BACKEND may
  spend one bit on a `bool` inside its own `writeBool` and flush at `endObject` — that is Cap'n Proto's win, it
  needs no attribute and no contract member, and it stays available. An author who wants bit-exact integer
  fields packs them into the smallest integer carrier in their own type, or hand-writes `serialize`. Measured
  on a `bool`+`uint8`+`uint8` record: hand-packing into a `uint16` costs 2 positional bytes, exactly what
  `@bits(1)/(3)/(7)` would have cost, against 3 for the natural fields. ⚠️ The carrier must be the SMALLEST
  that fits — the same record in a `uint32` costs 4, *worse* than not packing. The deciding argument was not
  size though: `@bits(4)` would mean four bits positionally, a whole byte in KBIN and a JSON number in text —
  one declaration with a wire form chosen elsewhere and never visible at the declaration, which is a hint, and
  GOALS favors explicit over implicit. It would also have cost two permanent contract members only one backend
  could honor.
- **Serde layering — SHIPPED `0.9.267`–`0.9.269` (2026-09-09).** The graph algorithm is
  `std::serialization::graph::ObjectGraph<T>`, a library type over the ordinary token protocol; the compiler
  emits per-node ADAPTERS against five prelude contracts (`GraphSerializable`/`GraphDeserializable`/`GraphRoot`,
  `GraphSerializer`/`GraphDeserializer`); the 8 envelope members left `Serializer`/`Deserializer`; the C
  substrate left `kama_runtime.h`; `Owned<X>` serializes inline (the triad's own conformance); the envelope is
  ordinary tokens, so **every backend carries a graph** (`ser_pos_graph`, `ser_num_graph`); the root is spelled
  `ObjectGraph<T>`. What the language surface now is lives in [SPEC.md](SPEC.md) § *Serialization*; the
  reasoning in the git log. Verdicts on what the design surfaced, each measured while building:
  - **A tagged-enum payload holding `Shared`/`Weak`** — *scheduled*: extend the enum derive with the adapter
    form (`writeNode`/`readInto`/`wireEdges` over the live variant's payload); mechanical now that the node
    adapters exist. Today refused with "`Shared` has none" (probed 2026-09-09).
  - **A collection OF graph nodes / edges** (`DynamicArray<Shared<X>>`) — ✅ **SHIPPED `0.9.277`/`0.9.278`**,
    and NOT the way this bullet predicted: it needs no edge protocol and no conformance on the collections
    at all. See *The graph channel* above. (`DynamicArray<Node>`, a collection of nodes BY VALUE, is a
    different thing and remains out — a node has identity, so it belongs in the table, not inline.)
  - **A collection OF `Owned<X>`** (`DynamicArray<Owned<X>>`) — ✅ **SHIPPED `0.9.276`**, and the language
    question this bullet posed was answered by `0.9.275` instead: nothing hops through a `const ref T` any
    more, so nothing asks whether one may name a smart pointer. What was actually missing was a
    `deserialize` for `Owned<X>` that is not a method on the triad.
  - **A fat `Owned<Contract>` field** — *scheduled*: the tagged inline form (`{tag: variant, value}` through a
    per-contract closed-world resolver over `@generate` implementors), the `Owned<X>` rule applied to a
    polymorphic pointee. Refused today with "`Owned` has none" (it was a graph edge before this row).
  - **Stable explicit type indices for graph nodes** — *optional*: `typeIndex` is the node type's position
    in the program's sorted node set, so adding a node type renumbers a positional/numbered graph wire; the
    named backends are the schema-evolution-safe ones, exactly as fields were before `@field(id:)`. A
    `@generate(index: N)`-shaped knob is the answer if a consumer asks.
  - **`ObjectGraph` over an `Owned<T>` root** — *non-goal*: an `Owned` tree is a by-value tree now.
  - **The stdlib `Weak<Contract>` does not monomorphize** (`weak.kama:36`, `tryUpgrade`'s `Shared.make` cannot
    infer its arguments over a contract element; no fixture ever used it) — a *bug*, its own row.
  - **A temporary passed to a contract-typed parameter** (`serializeJsonBuffer(v: ObjectGraph::<T>.of(…))`)
    takes the address of an rvalue in the emitted C — a *bug*, its own row; bind a local meanwhile.
  - **A generic ctor in argument position does not infer its type argument from the ctor's own arguments**
    (`f(v: ObjectGraph.of(root: r))` needs `ObjectGraph::<T>`) — the known "inference reads the destination"
    limit; recorded, not scheduled.
  - **A fat→concrete downcast** (`cast<Shared<T>>(fat)`) — a language question the design surfaced; not
    needed (root recovery is the synthesized `takeRoot`).
- **⛔ ARCHITECTURE REVIEW — 2026-09-09, and it blocks the graph-gaps row.** Work on a collection of edges
  stopped when the maintainer judged the shipped layering over-engineered. Everything below was **measured**
  against `0.9.269`; nothing was built. **It wants its own row — the maintainer should place it above the
  graph-gaps row.**
  - **The docs are wrong about reach.** `DynamicArray<Shared<Leaf>>` fails on the **by-value** path — *"field
    `kids` has type `DynamicArray`, which cannot be serialized … `DynamicArray` has none. Give it
    `@generate(Serializable)`"* — advice that cannot be followed, blaming the container when the ELEMENT is
    the cause. `computeReachesPointer` does not walk a LIBRARY collection's type arguments, so the owner is
    never a graph node. [SPEC.md](SPEC.md) § *Serialization* and the `computeReachesPointer` header comment
    both claim it does. Both are wrong, and the comment has claimed it since Phase A.
  - **The "collection OF nodes is not walked yet" message has nothing to do with collections.** It fires only
    for `Owned<Shared<X>>`, reporting the subject as `std::memory::Shared<Leaf>` — it calls a smart pointer a
    collection. `InlineArray<Shared<X>>` is refused earlier (an element must be a `value`), so no intrinsic
    collection of edges can exist at all.
  - **The adapters POLLUTE the user's type.** `head.typeName()` and `head.typeIndex()` compile from user code
    on a user's own `@generate` node type: 7 synthesized public members + 3 conformances, because a contract
    member must be `public`. Nothing in `tests/ examples/ bench/ lib/ prelude/ seed/` calls them outside
    `graph.kama`, so removing them is **not** a source break.
  - **`43dd3dd`'s outcome is right; its mechanism is not.** An `Owned` subtree IS a subtree rather than a
    table entry — keep that. But `Owned.serialize` is the first **domain** method ever put on the triad
    (`Owned`'s whole surface at `6dac7f0` was ctors + `deref`/`derefMut` + `release`), and the emitted C shows
    it **shadows** the pointee through the deref fallback — harmless today only because both write the pointee
    inline byte-for-byte. [SPEC.md](SPEC.md) bans shadowing at every binder; the deref fallback is not covered
    by that check. The compiler can walk through an `Owned` inline on its own (`graphNestOf` already answers
    `{"owned", X}`), so the conformance is unnecessary.
  - **`fab2c89` bundled two separable decisions.** **A** — the envelope became ordinary tokens, which is what
    bought positional/numbered graphs and simpler backends. **B** — the algorithm moved to the library, which
    alone produced the 5 contracts, the 7 public members and the `ObjectGraph<T>` ceremony. **A does not
    require B**, and the benefit that justified B — a library author writing a different driver — was measured
    at ONE consumer, a fixture deliberately forging a corrupt wire.
  - **The container seam is small.** `foreach` is lowered structurally (`iterator()`/`iterMut()` by name,
    direct monomorphized calls, no vtable) and `iterMut()` is ungated on `DynamicArray`/`Deque`/`FixedArray`/
    `View`, so the write and wire passes need **no contract**. Only read/**build** does — you cannot iterate a
    container into existence. Element receivers are proven: `xs[0].m()` and `foreach (ref T e in xs)` both
    compile, so **no language ruling gates the walk**.
  - ⚠️ **`private` on a contract member is a THIRD accepted-and-inert surface — measured 2026-09-09.** It
    parses, and the marker is silently dropped: with the member declared `private` on the contract and the
    implementer `public`, a call through a contract-typed value (`ref Tok t; t.hidden();`) compiles clean.
    So the forced-`public` rule is load-bearing **only because the other half was never built** — nothing
    checks a contract member's declared visibility at the call site. There is exactly ONE enforcement site
    today (the conformance check that refuses a non-`public` implementer); the contract declaration itself
    accepts `private` without recording it. That is the same defect class as `friend`-on-contract before
    `0.9.266`, and it wants an `xfail` regardless of whether the feature lands.
  - ⚠️ **SUPERSEDED — the container seam needs no contract.** Restricted-private contract members were
    proposed as the mechanism for a checked read/build seam. Building the architecture out showed the seam
    is not there: a container reads itself through its own `deserialize`, which already knows how to build
    one, so nothing needs a private member. Row 8 keeps its place on expressiveness alone. The bullet below
    is kept for provenance.
  - **Direction: restricted-private contract members.** The forced-`public` rule exists only because a public
    member with a private implementer is "reachable through the interface but not by name: a leak"; a
    **private** contract member is self-consistent, so **mixed contracts are fine — public members stay fair
    game**. `private` with **no grant** is "only the compiler may call it", with no new concept: the emitter
    writes C directly, and users may still IMPLEMENT it, which keeps third-party containers extensible. One
    rule changes: an implementing method matches the member's declared visibility. ⚠️ This **reverses** the
    `0.9.266` removal of `friend` on a contract — that removal was right (it was accepted and INERT), so
    `tests/xfail/friend_on_contract` and its doc claim change in the same commit and the new rule must fail
    closed on both holes. `friend X[members]` already grants fields, methods and ctors alike (measured).
  - **A fourth visibility (`internal`/`compiler`) — genuinely OPTIONAL, not scheduled.** `internal` is already
    taken at module level (package scope). The only gap it closes over `private`-with-no-grant is stopping the
    declaring type from calling its OWN member, and kama polices that nowhere. Revisit only if protocol misuse
    actually bites.
  - **Open questions, ANSWERED 2026-09-09 by probe:**
    - **Is `ObjectGraph<T>` load-bearing after `43dd3dd`? NO.** A pure-`Owned` tree round-trips **by value**
      today with no `ObjectGraph` anywhere, re-encoding byte-identically — which is the case that originally
      motivated the wrapper. What remains is only the graph-root spelling, and `Shared<T>` as a root is
      refused **by choice** (`tests/xfail/graph_root_shared`, *"a graph root is `ObjectGraph<T>`"*), having
      been the working spelling at `6dac7f0`. So the ceremony is revertible.
    - **Can a compiler-owned driver emit the token envelope with no `Serializer` graph members? YES, by
      construction.** `graph.kama` already writes the whole envelope in ordinary tokens
      (`beginObject`/`field`/`writeU64`/`beginArray`/`variant`/`endObject`) from library code, so emitted C
      can make the same calls. **Decision A survives a compiler-owned walker.**
    - **Does `Map` expose a mutable VALUE iterator? YES, and ungated** — `valuesMut()`
      (`map.kama:266`), exactly like `iterMut()` on the sequence containers. Value-position edges are
      walkable; key position stays impossible by bound.
    - **Does the wire walk compose for nested containers? YES** — a nested mutable `foreach`
      (`foreach (ref DynamicArray<int32> inner in outer) { foreach (ref int32 x in inner) … }`) compiles and
      runs, so a compiler-emitted wire pass can nest.
    - **Can a synthesized conformance attach to the intrinsic fat `Shared<Contract>`? MOOT** — the corrected
      design keeps the triad method-free and recognizes edges by identity, so nothing is attached to it.
- **More back ends (library, no compiler change)** — YAML; **XML**/**HTML**. Each is a `Serializer`/`Deserializer`
  impl + `serializeJsonBuffer`/`deserializeJsonBuffer`. (`std::encoding::base64` shipped `0.9.197` as its own small module, with
  `::hex` beside it — SPEC § *Encoding*.)
- **A back end's ENTRY POINTS are a convention, not a contract** — the defect the line above quietly
  describes. `Serializer`/`Deserializer`/`Serializable`/`Deserializable` are real contracts
  ([prelude/global.kama:207](../prelude/global.kama)), but `serializeJsonBuffer`/`deserializeJsonBuffer`/`deserializeJsonStream` are **bare free
  functions**, duplicated per back end (`json.kama:198,538,546`, `binary.kama:253,263,270`) with nothing
  checking that a back end supplies them or that their signatures agree. "A drop-in twin of the JSON back
  end" is true only by discipline. Wants a `Format` (or `Codec`) contract carrying the three, so a back end
  is a checked implementation. It is also the source of the **one** name collision in the flattened-stdlib
  measurement (`serializeJsonBuffer`, json vs binary) — it surfaced while measuring a flattened stdlib for the module campaign. Take it with the std-lib cleanup pass, not before.
- **Serde naming — SHIPPED `0.9.292`.** Every level is one axis now and every leaf is a format:
  `std::serialization::text::json` and `std::serialization::binary::kbin`, with
  `KbinNamedSerializer`/`KbinNumberedSerializer`/`KbinPositionalSerializer` beside `JsonSerializer`.
  Addressing qualifies a name only where there IS a choice — `kbin` has three, JSON's is inherently named.
  ⚠️ The rule it establishes, for whoever adds the next back end: **a module names a FORMAT; the medium is
  the level above it.** `binary` was squatting on the medium name because kama's binary format had none,
  which is also why the type names mixed axes. `…Writer`/`…Reader` had gone earlier for colliding with the
  `std::io::Writer` sink the type owns.
  **Successors the maintainer has in view:** `text::yaml` (addressing inherently named, so `YamlSerializer`
  with no qualifier, exactly like Json) and `text::csv` — ⚠️ **csv is not shaped like the others and needs a
  decision before it is built, not during**: it is flat and tabular, so a `Serializable` with a composite
  field has no CSV representation at all. The maintainer's steer is to *limit it to the shape it is* — a
  backend typed to a row shape it takes and returns, rather than a general `Serializable` backend that
  refuses most types. It also has an addressing axis of its own (header row = named, none = positional), so
  it would take the `{Addressing}{Format}` form `kbin` uses rather than the bare one.
- **`@deprecated` as a general declaration marker — NON-GOAL** (maintainer, 2026-09-11). The FIELD meaning
  is shipped and is the whole of what was wanted: read when present, never written, name and id still
  reserved for duplicate detection (verified — an old stream's field reads back, a new write omits it).
  A general marker on a method or type was the other half, and it is refused on two counts. The
  maintainer's: *"that is what comments or the delete key is for."* And a structural one — it was specified
  as a **use-site warning**, and kama deliberately has no stderr warning channel: a soft `warning()` was
  REMOVED, with `run_tests.sh` failing any fixture whose stderr matches /warning/i, "because a warning is
  the compiler saying it does not believe its own output". Building it as specified would trip the harness
  by design. (`DiagSeverity::Warning` survives for LSP-only diagnostics, so an editor-only deprecation hint
  remains possible if it is ever wanted — but it is not this row.)
- **Optional/default *function/constructor* parameters (language, adjacent)** — the "options struct with
  optionals" ctor pattern. A **non-goal**, settled by the audit: one way to do a thing (GOALS 4) — named static factories + named params cover it, and SPEC § *Generics* says the same.
  (Distinct from **default *type* parameters**, which shipped.)

<a id="s5"></a>

## 5. 1.x — systems & runtime (post-1.0)

Capabilities built on the finished language — the substrate the engine needs (asset I/O, scene serialization,
networking). The MCU/embedded language surface and the const-eval ladder are done ([SPEC.md](SPEC.md),
[MCU_READINESS.md](MCU_READINESS.md)). Remaining forward work:

### The allocation campaign (KR-47 – KR-50) — opened 2026-09-12

The design, the measured inventory of every allocation site, and the order live in
[docs/design/allocation.md](design/allocation.md). In one paragraph: `kama_alloc`/`kama_free` become the
only way heap memory is obtained or released, delegating to a global allocator that defaults to
`malloc`/`free` and that a program can replace; every raw site in emitted C, the runtime headers, the OS
seam and the prelude moves onto them, which also ends the ~20 blocks allocated by one family and freed by
another (correct today only because every family is libc); error boxing draws from an allocator like every
other box; and `--no-heap` judges what the program REACHES, consistently, rather than every imported body.
It meets KR-39 (a provable callee behind a contract slot) and tier 1 of the devirtualization ladder (KR-23):
both are "judge what is actually reached", and they should share one reach walk.

- **Reflection + declarative serialization** — see §4; back ends follow as modules. Rides on the shipped
  `std::fs`/`std::io` for asset + scene load.
- **Container / data-structure reach — honest non-goals.** The core containers ship (`DynamicArray`/`FixedArray`/
  `InlineArray`/`string`, `Map`/`Set`, `Deque`, `PriorityQueue`, `SlotMap`, `BitSet`, `SortedMap`/`SortedSet`,
  `View<T>` — see SPEC). **Not** planned as stdlib: general **linked lists** (mostly a cache anti-pattern in
  data-oriented engines — the useful form is an intrusive free-list / LRU); raw **BSTs** (subsumed by the sorted
  map); **spatial trees** (quadtree/octree/BVH/k-d — engine-specific).
- **Collections revisit — remaining knobs & optimizations.** The parametric knobs ship (preallocation,
  pluggable `Hasher`, custom `Allocator` on every container and box, all defaulted). What remains:
  - **HashDoS-resistant keyed hashing (deferred).** `DefaultHasher` is deterministic/*unseeded* — right for
    trusted keys but NOT resistant to attacker-chosen keys. A seed at the `finish` stage can't fix this (it
    would leave string keys' unseeded FNV-1a content hash exposed). Real resistance needs a **seeded, keyed hash
    over the key bytes** (SipHash-class): the seed enters the per-byte accumulation + OS entropy + per-map seed
    storage. It **rides the pluggable `Hasher` seam non-breakingly**, so it's a clean future milestone — do NOT
    ship a finish-stage `SeededHasher` (misleading safety for the case that matters).
  - **Zero-size-field elision — deferred optimization.** The default `Owned<T>` carries a `GlobalAllocator alloc`
    field (mirroring the collections), which pads the handle. A general "drop any empty-struct field + synthesize
    a throwaway receiver for method calls on it" pass would reclaim it on `Owned` *and* every collection at once.
  - **Store-once allocator / thin smart-ptr handles — deferred optimization (Rust `Arc<T,A>` model).** The
    smart-pointer family carries `A` **per handle** (a small copyable value handle). The memory-optimal
    alternative stores the allocator **once** in a monomorphized control block and keeps handles thin. Not taken
    because `Owned` has no control block (can't unify), it would reopen the shipped concrete path, and the
    savings are small (the handle is already lightweight). Revisit as a whole-family refactor gated on profiling,
    bundled with the elision pass above.
- **Browser networking transports** — native TCP ships (`std::net`); the browser has no raw sockets, so the wasm
  path needs **WebRTC DataChannels** (unreliable) / **WebSockets** (reliable) via a host FFI shim. Native
  UDP/DNS and the rest of the stdlib reach are the §1 follow-ups.
- **Native dispatch devirtualization** *(optimization, not a gap).* On a *monomorphic* call site clang does not
  devirtualize the emitted C vtable while rustc does — a clang-vs-rustc optimizer gap (hand-written C is equally
  behind), not a kama defect. kama can win where it *sees* the concrete type by emitting a **direct call** — a
  laddered pass:
  - **Tier 1 — sound static devirtualization (no inlining).** Direct-call when the target is provable: a
    concrete-value receiver, a `final` class/method, or a method with no overrides program-wide (a slot→overridden
    map after `buildVtables()`). kama's whole-program view makes the last one free where C++ needs LTO +
    `-fwhole-program-vtables`. Land this first.
  - **Tier 2 — intraprocedural type-flow.** Devirtualize a base-typed local with a proven concrete assignment.
  - **Tier 3 — inlining-enabled / guarded devirtualization.** A kama-level inliner (hard part: integrating callee
    scope-cleanup / drop order / move-state with `emitScopeCleanup`) then re-run Tier 1, or guarded inline caches.
    A separate, larger project — pursue only if a real hot path (engine ECS dispatch) proves Tier 1 insufficient.

### Embedded / bare-metal MCU (Pi Pico · Arduino · ESP32) — toolchain packaging

The language surface is done ([MCU_READINESS.md](MCU_READINESS.md)); what remains is build and library work,
across two different targets:
- **Raspberry Pi (Linux — Pi 3/4/5, Zero):** a full ARM app processor running Linux, which kama already
  cross-compiles to. What is left is **GPIO/I²C/SPI bindings** — ordinary C FFI over `libgpiod` / `/dev/mem`.
- **Bare-metal MCU (Cortex-M: Pi Pico/RP2040 · Arduino Zero/Nano 33, ESP32; later AVR):** *freestanding* — no OS,
  KB of RAM, often no heap, a startup file + linker script instead of hosted libc:

  | Piece | What's needed |
  |---|---|
  | **Toolchain / build** | The turnkey Cortex-M path ships and is QEMU-proven ([mcu.md](mcu.md)). **Remaining:** more board presets (STM32/Pico), vendor-HAL glue (pico-sdk / esp-idf), a real-hardware flash pass, and (optional) folding the two-step link into `kama build --target <board>`. Arduino `setup()`/`loop()` is a later HAL nicety. |
  | **AVR (Harvard) family** *(deferred — Cortex-M/RISC-V first)* | Four AVR-specific pieces: (1) ISR — `@interrupt("VECTOR")` → the `ISR(VECTOR)` macro (`<avr/interrupt.h>`), not the parameterless `__attribute__((interrupt))`; (2) Harvard `PROGMEM` — flash const data needs `PROGMEM` + `pgm_read_*` accessors (a flash pointer can't be plain-deref'd), so `@section` alone doesn't cover it; (3) toolchain — `avr-gcc`-only (clang/zig don't target AVR cleanly); (4) **`-mdouble=64` in the target's `cflags`** — avr-gcc still defaults to a 32-bit `double`, which `kama_runtime.h`'s `_Static_assert` rejects. It is the ONLY target in kama's spectrum that fails those asserts, and the assert is doing its job: without it, `bitcast<uint64>(d)` would pun an 8-byte union member against a 4-byte one and `kama_f64_bits` would `memcpy` 8 bytes out of a 4-byte `double`. A config line, not a language gap. |

  **Why kama fits:** no-GC + RAII → deterministic, no hidden pauses; allocation is explicit in the emitted C
  (greppable no-heap audit); trap lowering is dependency-free; `InlineArray<T>#(N)`, sized ints, and `unsafe`/`UnsafePtr`
  FFI already exist. **North star: blink an LED** (the embedded "first triangle"). **Start Cortex-M, not AVR**
  (`zig cc`/clang do `thumbv*-none-eabi` cleanly; pico-sdk is tidy; AVR pain comes later).

### Compile-time evaluation & platform-specific compilation — residuals

The const-eval ladder and decl-level conditional compilation are done ([SPEC.md](SPEC.md)). What is left:

- **Host-endianness flag → `htole`/`htobe` (small).** `std::num` `byteswap*` (pure value swaps) + `bitcast`
  ship, but *host-order* serialization helpers need a compile-time endianness fact pure kama arithmetic can't
  observe — a natural `@compileFor`-style built-in flag (`LITTLE_ENDIAN`/`BIG_ENDIAN`). Every current target is
  little-endian, so this is deferred until a big-endian target appears.
- **`comptime fn` nice-to-haves (deferred).** Named-arg reorder *inside* a comptime fn body; a **local**
  `comptime T X = f();` initialized by a comptime-fn call (module + type-associated const forms ship); a
  per-fn `@steps(…)` budget override; dual-use fallback emission. (`sizeof` inside a comptime fn body ships
  with M6 — `alignof` does not, and that is now a rule rather than a gap: see §2's layout entry.)
- **Platform tag-type compilation.** The `@compileFor`-gated contract-impl seam is the sanctioned platform-variance
  mechanism (per-platform `type` impls behind a platform-agnostic `contract`, exactly one survives) — NOT
  in-function branching / `#ifdef`. Extending it as new targets land is forward library/driver work.

<a id="s6"></a>

## 6. Concurrency — what is left above the primitives

The language primitives are done ([SPEC.md](SPEC.md#concurrency-)). The higher-level **job system and
event-loop scheduler are libraries** on them (Go/Erlang-style block-on-channel, deliberately **not**
`async/await` function-colouring) — see the engine track (§8) and
[WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md).

**The pool the job system is built from can now be sized to the machine** — `cpuCount()` plus
`parallel_spawn` shipped 2026-08-29 ([SPEC.md](SPEC.md#concurrency-)), so the library can construct itself
at the right width instead of hardcoding one. What remains for that row is scheduling, not spawning: a
fixed pool where one job blocks ties up 1/K of capacity no matter how K was chosen, and **head-of-line
blocking is the job system's problem to solve.**

- **Deferred (reopen only on a concrete case) — general shared-memory ("hybrid").** Co-equal shared-memory
  threading is *not* planned; it reintroduces the hazard the model removes. Capability is retained (via the
  `Atomic<T>` seam + immutable-`Shared` + disjoint `parallel_for`); only some ergonomics move behind the seam.

<a id="s7"></a>

## 7. 2.0 — dual-mode: compiled + scripting/REPL (flagship)

**Sequenced AFTER the LSP** (§1 post-1.0 order; user, 2026-07-26). **Strictly additive — it never touches the
native/release C tier, which stays at C parity.** Direct-WASM is "another hot-reload/scripting option" (near-native,
toolchain-free) — *not* as fast as native kama today, so the C→emcc release path stays the max-perf web route.

**One language, two modes** — the *same static kama* (identical syntax, semantics, ownership rules; dynamic only
in *execution*, never in typing) usable both compiled and as a scripting language with a full REPL. The target is
a REPL that **replaces the Python/Ruby/Lua REPL** for fast iteration and compile-→-run-on-demand, at or near
native speed. Guiding constraint: **the `kama` binary is the only tool you need** — external C toolchains stay
*optional* (the portable-C release path), never required to write, run, or iterate.

**Two tiers, chosen by what dominates:**

| Tier | Path | Optimized for |
|---|---|---|
| **Release / AOT** | `kama → C → clang`/`emcc` | maximum runtime speed, the portability moat |
| **Iteration / scripting / REPL** | direct-wasm (+ `wasm-opt`), then a bytecode VM | compile speed, zero external toolchain, interactivity |

The release tier ships today and is untouched; the new work is the *iteration* tier, and it is **additive** —
never a replacement for C.

```
   Frontend  (parser → type checker → ownership/move analysis)   ── safety proven ONCE
                          │
                   semantic lowering   (monomorphize, insert drops,
                          │             desugar vtables / match / operators)
          ┌───────────────┼────────────────────┐
          ▼               ▼                      ▼
    C (clang/emcc)   direct WASM            Bytecode VM
    RELEASE — max    + wasm-opt             REPL, self-contained
    speed, moat      ITERATION / web        (IR extracted here, if ever)
```

Every backend shares the same front end, so the safety analysis is proven **once**, before lowering.

**Toolchain packaging rides along.** The version store is already modality-aware (`versions/<kind>-<v>/`,
`kind=compiler`), so once a scripting runtime exists it becomes the second `kind` — `kama toolchain` gains
runtime versions alongside compiler versions, resolved by the same project pin. Nothing to build until the
runtime does; it is only listed here so the store's spare axis isn't forgotten.

**Sequencing — polymorphic emitter first, a shared IR only when the VM forces it.** The emitter
(`kama.cemit.*`) *bakes in* the semantic lowering (monomorphization, RAII drop insertion, vtable layout,
match/operator desugaring). A "shared IR" is just that lowering **factored out** into a data structure the
backends consume — so *polymorphic emitter* and *shared IR* are the same idea at two points on a spectrum. The
pragmatic path:

1. **Refactor the emitter to an abstract interface**, C as the first implementation, the shared lowering in the
   base. Low risk.
2. **Add a direct-wasm backend** as a sibling — same lowering, different rendering. Drops the `emcc` dependency
   for self-contained web/scripting builds and proves the seam. (C→emcc still produces the maximal-compatibility
   release wasm.)
3. **Extract an explicit IR only when the VM needs it** — a bytecode VM is a genuinely different execution model,
   so re-deriving the lowering a *third* time is where a shared lowered form actually pays off. The VM becomes a
   low-drift renderer of the *same* lowered form the C backend uses.

**Design constraints (hold across the whole spectrum):**
- **Keep the lowered form high-level and structured** (retain `if`/`while`/`for` and named locals), *not*
  SSA/basic-blocks — so the C backend still emits the readable, `#line`-mapped C that is a headline feature.
- **Move the runtime into kama.** Collections/smart-pointers/`string` live as hand-tuned C in `kama_runtime.h`;
  a wasm or VM backend can't `#include` it. Finishing the port makes multi-backend and the
  kama-stdlib/self-hosting goal the **same** project — do it once, all backends inherit it.

**Direct-wasm optimization — lean on Binaryen, don't write an optimizer.** A naive direct `kama → wasm` backend
emits ~`-O0`/`-O1`-quality code. The fix is **`wasm-opt`** (Binaryen), a standalone optimizer that runs on *any*
wasm regardless of producer (the AssemblyScript model). `kama → wasm → wasm-opt -O3` recovers most of the gap. It
is **not** equal to `C→emcc -O3` (LLVM's mid-level IR optimizer + SIMD autovectorization stay ahead — heavy
numeric loops still favor the release tier), but buys **compile speed + zero dependency + a REPL** at
near-native runtime.

**Speed ladder** (fastest last): tree-walk < bytecode VM < direct-wasm/`wasm-opt` < AOT C→clang.

- **Licensing.** **Binaryen (`wasm-opt`) is Apache-2.0** — clean against the MIT goal (GOALS #8). A bundled
  **TinyCC**-JIT (near-instant native `kama run`) is a possible *optional* alternative but is **LGPL** — confirm
  linking terms before shipping it; the VM / direct-wasm paths sidestep it entirely.

<a id="s8"></a>

## 8. Engine track (product north star)

A portable lightweight **WebGPU** game engine — a product built *on* kama, **not** part of the language. Tiers:
**math types** (shipped) → buffers/bindings → first triangle (shipped, browser + native, `examples/webgpu`) →
scene/material. Depends on the 1.x systems (file I/O for assets, serialization for scenes). The kama-scoped
remainder is a thin safe `std::gpu` binding wrapper over the shipped `kama_gpu.h` seam; the engine *spine*
(buffer/pipeline/binding libraries, renderer) is the engine product. See
[ENGINE_READINESS.md](ENGINE_READINESS.md).

- **Safe `std::gpu` binding wrapper — no longer "optional polish".** `lib/std/gpu` holds a C seam and *no
  kama at all*. The wrapper is the missing kama file, shaped like `std::net`'s `type resource TcpStream
  { isize fd; }`: `Device`, `Surface`, `Buffer`, `Texture`, `TextureView`, `RenderPipeline`, `BindGroup`,
  `CommandEncoder`, each an `UnsafePtr` handle whose destructor calls the matching `wgpu*Release`. Plus a
  `Result<SurfaceTexture, SurfaceError>` acquire whose error enum **names `Occluded`**, and the two seam
  holes below (drawable size; the discarded event queue).
  - **Why it was promoted.** The first external project's KB-5 — an unbounded-allocation frame loop that
    reached 15.6 GB and took a machine down twice. ⚠️ **Be precise about which half would have caught it:**
    RAII would *not* have. That allocation is inside wgpu-native, triggered by ignoring an untyped status
    int; what prevents it is the **typed acquire result**. RAII prevents the *other* leak in the same file
    — `examples/webgpu/triangle.kama` hand-writes **five** `wgpu*Release` calls per frame, all on the happy
    path, so any early `return` added later leaks them. Two different bugs, two different halves.
  - **Scope line.** Exactly the handles the seam and the triangle already touch, and nothing above them.
    Buffers/bindings/pipelines *as an engine renderer* stay out; the same things *as released handles* are
    in. Bindings *breadth* is the engine's job, not the language's.

- **Dev-loop hot-reload — a *library* on two small compiler primitives that already ship.**
  - **Compiler primitives (ship — see SPEC *Exposing to a host*):** `kama build --shared` (`.so`/`.dylib`/`.dll`)
    and the `expose` keyword's C-ABI linkage are the *same* kama→host boundary the wasm exports and the scripting
    host (§7) use — so hot-reload needs **no new language surface**. One boundary, three consumers.
  - **Library:** the `dlopen`/`dlsym`/`dlclose` + file-watch + function-pointer rebind loop — pure FFI over
    `unsafe`/`UnsafePtr`, zero compiler changes. This is the bulk of the feature. (A Windows copy-before-load, so the
    on-disk `.dll` can be rebuilt while loaded, is a library concern.)
  - **Engine:** the *data-in-host, code-in-module* architecture (world state lives in the platform-layer arena,
    passed *into* the reloaded module) so a reload doesn't wipe the world. Prior art: Handmade Hero, Our
    Machinery, Unreal Live++, Godot GDExtension, Bevy `hot_lib_reloader`.
  - **Scope — desktop dev only.** dlopen is absent/forbidden on ship targets (no wasm dlopen; banned on iOS;
    Android/Quest only via a pushed `.so`). Cross-platform *shipping* scripting is the §7 VM, not this. This buys
    fast native iteration on Linux/Mac/Windows — enough to justify the two tiny primitives.

<a id="s9"></a>

## 9. Performance

Where kama currently stands is measured in [benchmarks/RESULTS.md](benchmarks/RESULTS.md) — read it there
rather than here, so there is one number to keep current. Forward work:

- **Bench methodology (don't re-chase).** Measure wasm at the optimizing tier (`node --no-liftoff`). Short
  workloads skew under parallel load — run with nothing else competing. Keep all LLVM-AOT languages at the same
  `-O` level (`-O3`), or the optimization level dominates a tiny kernel.
- **Bench cohort — add Zig.** kama's closest *language* rival (no-GC, AOT, and, as `zig cc`, already kama's
  bundled backend). Port the workloads to `.zig`, add the toolchain. Expect it to cluster with C/Rust on raw
  compute — the signal is the *compile-time / binary-size / RSS* columns and cohort completeness, not the perf
  ranking.
- **Serialization benchmark track.** A headline feature — add a round-trip workload, scoped honestly (it measures
  *library maturity + reflection-vs-compile-time strategy*, a different axis than the compute kernels). Only 6 of
  11 bench languages have stdlib JSON. **v1:** a by-value tree round-trip across the stdlib-JSON six —
  *intrinsic (kama)* vs *runtime-reflection (Go/C#)* vs *interpreted (Python/JS)*. **Document, don't race, the
  object graph** (kama's shared/`Weak`/`Owned` graph serde has no equivalent — a capability note, not a number).
- **Compile-time / suite-time.** Measured 2026-08-09/10 on a 10-core M-series host. The whole record is
  below; there is no separate design doc any more.

  **Shipped, in order of when the evidence justified it:**

  | | change | measured |
  |---|---|---|
  | 1 | `./dev matrix` stopped running the guard block twice (`KAMA_SKIP_CHECKS`) | −61 s from `matrix` |
  | 2 | one parallel guard runner, `tools/run-checks.sh`, glob-enrolled | guards 61 s → 38 s |
  | 2½ | `make -j` | cold compiler build 8 s → 3 s |
  | 3.1 | `kama query` takes an ordered question list — one analysis, N answers | `check-query` 20.9 s → 6.0 s |
  | 3.2 | `kama check --each` — N programs in one process, sharing prelude + import closure | agreement 22 s → 10 s |
  | **5** | **`kama build -j` — the C compiles run concurrently** | **user builds ~2× (below)** |
  | 6 | the harness itself: fifo-semaphore gate, per-run seam sweeps, multi-file leg fanned out | −30 core-s, −9 s wall |
  | **7** | **`-O2` on the compiler itself — it had never been optimized** | **front end 8.2× (below)** |

  `./dev test` **184 s → 122 s → 104.6 s**; guard block **61 s → 14 s**.

  ### Lever 7 — the compiler was built at `-O0`, and had been forever

  **The single largest lever in this section, found last, by taking a measurement the closure-pruning
  brief demanded before any code was written.** `Makefile`'s `CXXFLAGS` carried **no `-O` flag at all** —
  not in the Makefile, not in `tools/`, not in `dev`, not in either CI workflow. Every kama binary ever
  built ran unoptimized, **including the ones `release.yml` shipped**.

  It hid because the `-O` flags anyone looks at are the ones `kama build --release` hands the C compiler
  for the *user's* program (`-O3` native / `-Oz` wasm, `kama.driver.cpp`). That is a different codebase one
  level down, and "we do optimized builds" was true the whole time — about the other one.

  Measured (`OPT ?= -O2`, `examples/httpd`, same host). **Taken when httpd was 32 units; closure pruning
  has since made it 10, so these absolute numbers no longer reproduce** — the ratios are the claim:

  | | `-O0` | `-O2` |
  |---|---|---|
  | `kama check` front end | 338 ms | **41 ms** (8.2×) |
  | ├ closure-parse (32 units) | 174 ms | 13.7 ms (12.7×) |
  | ├ prelude-parse | 28 ms | 2.2 ms |
  | └ analyze | 135 ms | 25.1 ms (5.4×) |
  | `kama build -j 10` | 0.67 s | **0.37 s** (1.8×) |
  | `kama check --each tests/*.kama` (597 files) | 25.3 s | **4.65 s** (5.4×) |
  | `./dev test` wall | 122.0 s | **104.6 s** (1.17×) |
  | `./dev test` **CPU** | 391 s | **197 s** (**1.99×**) |

  **The suite halves in CPU but drops only 17 % in wall clock**, because it is already parallel and its
  remainder is external `clang`, running the fixture binaries, and bash — exactly what lever 6's
  decomposition predicted. CI, on 2-4-core runners, is CPU-bound and should see much more of the 2×.

  Correctness, in the order the evidence was taken — **the optimized compiler emits byte-identical C**:
  every `.c` and the `.gen.h` for `examples/httpd` compare equal, and `kama check --each` over all 597
  fixtures produces byte-identical diagnostics. `-Werror` is clean at `-O2`. A one-off **ASan+UBSan build
  of the compiler itself at `-O2`** ran the whole suite green (974 passed, 26 checks, zero sanitizer
  reports) — worth knowing because `./dev test san` sanitizes the *emitted programs*, never the compiler,
  so nothing else in the gate covers UB in the compiler's own C++.

  `-O1`/`-O2`/`-O3` landed within noise of each other (41.4 / 41.1 / 42.2 ms) and `-O3` built *and* ran
  slower, so there is nothing above `-O2` to chase. Kept overridable (`make OPT=-O0`) for compiler
  debugging and held by [`tools/check-opt.sh`](../tools/check-opt.sh). The from-scratch compiler build
  costs more now (8 s → 23.5 s serial, 3 s → 12.7 s at `-j10`), which is why both CI workflows build with
  `-j4`.

  ⚠️ **Everything measured before 2026-08-10 was measured against an unoptimized compiler.** The rows above
  are re-baselined; anything quoted elsewhere from that period is not. **`benchmarks/RESULTS.md`'s runtime
  columns were unaffected** — the bench builds `--release`, so the *emitted* code was always `-O3`; only its
  compile-time column measured the compiler. Re-run in full 2026-08-10: **kama 1227 ms → 908 ms** for the
  9 bench binaries, i.e. **3.38× C per binary → 2.33×**, while every other language in the cohort drifted
  5-19 % *slower* in the same run. The runtime rows moved 3-5 % together, which is that run's ambient
  noise, not a change — kama stays at C parity on compute. Since kama's figure is transpile-to-C **plus**
  clang, it can never beat C; 2.33× means the front end now costs about a third again what clang costs on
  the same code.

  ⚠️ **The LSP per-keystroke floor below (~85 ms, 86 % `analyze`) is a pre-`-O2` number and has NOT been
  re-measured.** `analyze` alone got 5.4× faster, so the floor certainly moved; the split is not published
  here again until someone takes the measurement rather than deriving it.

  **Lever 6 is where the suite's remaining time actually was, and it is not the compiler.** Decomposing
  the 82 s fixture phase: building all 597 standalone is 33.8 s, running the built binaries is 24.6 s, and
  ~22 s is bash. Cutting at that got −30 **core**-seconds but only −9 s of wall, because the old polling
  gate's latency overlapped with the other nine workers — a prediction of ~12 s that was simply wrong.
  What remains of the 22 s is spread across one `mkdir` and one warning-`grep` per fixture, the subshell
  each job needs, and the result writes. Nothing left there is worth a commit.

  **Lever 5 — `-j`, and it is the only one users feel.** A C compiler handed N sources in ONE invocation
  compiles them **serially**, and a program importing anything from `std` was 16-32 TUs at the time (a
  directory-module import then pulled in every file in the directory — closure pruning has since cut that,
  see below), so `kama build` used one core for ~70 % of its wall time.
  Now each TU is its own `-c` job and the objects are linked. Measured on `examples/httpd`, **then 32 TUs
  and now 10, so these absolute numbers no longer reproduce** — the ratio is what the lever claims:
  C phase 0.93 s → 0.27 s + 0.02 s link (**3.2×**), whole build **1.27 s → 0.67 s (1.9×)**. Degrades
  gracefully — ~1.3× on a 2-core machine — and gains exactly nothing for an import-free program (1 TU).
  Guarded by [`tools/check-build-jobs.sh`](../tools/check-build-jobs.sh); the harness pins
  `KAMA_BUILD_JOBS=1` because its own pool is already core-wide, which also keeps the suite covering the
  single-invocation path. `-j 1` is that path byte for byte.

  ⚠️ **`-j` does NOT move the bench's compile-time column, and should not be expected to.** That column
  builds `--release`, which folds the whole program into one unity TU — there is nothing to split.
  Measured over the 9 bench workloads: `--release` is **1.71 s at both `-j 1` and `-j 10`**, while the
  same nine in **debug** go **2.88 s → 2.04 s (1.41×)**. The win is a dev-loop win, and the bench measures
  release artifacts. If a number for the edit-compile loop is ever wanted, it needs its own debug row —
  don't "fix" the release one.

  **What is left is a product feature, not a suite lever — and two premises died proving it.**

  - **`zig cc` already does incremental rebuilds, so lever 4 (kama's own object cache) is mostly moot for
    bundled installs.** Measured, 32 TUs: one invocation is 4.13 s cold, **0.07 s warm, 0.11 s after
    editing one file** — it has a content-addressed per-TU object cache and re-compiles only what changed.
    That is strictly better than the cache §4 of the old brief designed (no cold-CI penalty, no `gen.h`
    invalidation problem). Per-TU `zig cc` cannot use it (bounded by zig's ~0.18 s process startup: 0.59 s
    cold *and* warm), which is why `-j` clamps to 1 for zig — parallelizing there would be 7× better cold
    and **5× worse in the edit-rebuild loop**. A slim install (the installer's choice whenever a system C
    compiler exists) uses clang, which has no cache, and gets the full `-j` win.
  - **An on-disk front-end cache does NOT fix the LSP's per-keystroke floor**, which is what the old brief
    claimed was its main justification. The LSP already caches parses in-process
    (`kama.driver.cpp` `g_parseCacheMap`); its steady state was **~85 ms/keystroke, 86 % of it
    `CEmitter::analyze`** over the whole closure plus prelude — ⚠️ **a pre-lever-7 number, now stale and
    not re-measured.** Serializing the AST and re-running collect leaves that untouched either way, which
    is the point that still stands. Whether the floor needs *incremental or cached analysis* is now an
    open question rather than a settled one — see §10.
  - **Declined: `kama build --each`** (batch the fixture builds in one process). Priced at ~9-10 s off a
    129 s suite for a 377-line refactor plus a harness restructure, and it gives users nothing. Not worth
    it; recorded so it is not re-derived.

  **Deliberately NOT taken: folding a build to a single TU.** The biggest raw number (24 TUs 0.52 s → 1 TU
  0.08 s, same binary) but it changes what kama *emits*, and multi-TU emission is load-bearing — the
  logging campaign fixed a whole multi-TU `static` hazard class (`adfffce`) that a single-TU suite would
  stop exercising. (`--release` native already folds to one unity TU, for cross-module inlining.)

- **Closure pruning — SHIPPED 2026-08-10.** A directory-module import used to compile the whole
  directory: `import { std::collections::DynamicArray };` pulled in all 14 files of `lib/std/collections`,
  because `resolveModuleFiles` falls back to a flat listing and the `{…}` names control *visibility*, not
  what gets compiled. `examples/httpd` named four imports and got 32 TUs, 20 of which contributed no live
  symbol. An import now resolves to the files defining the named symbols plus their transitive
  intra-directory closure. Resolution rule: [SPEC.md](SPEC.md#module-resolution). Guard:
  `tools/check-closure-pruning.sh`. Escape hatch: `KAMA_NO_PRUNE=1`, plus `KAMA_PRUNE_TRACE=1|2` for
  per-import decisions and the reference that pulled in each kept file.

  | examples/httpd | before (32 TU) | after (10 TU) | saving |
  |---|---|---|---|
  | `kama build -j 10` | 390 ms | **168 ms** | **57 %** |
  | `kama build -j 1` | 1011 ms | 355 ms | 65 % |
  | `kama check` front end | 43.9 ms | 29.2 ms | 33 % |
  | └ `analyze` | 26.5 ms | 14.2 ms | 46 % |
  | └ `closure-parse` | 14.9 ms | 12.8 ms | 14 % |

  Well past the ≥ 25 % rule fixed before the measurement, and past the 40.5 % the design brief predicted
  from a 32 → 17 hand-prune. The derived closure reaches **10**, and even the parse got cheaper: a module
  no kept file imports is never indexed at all. That also repaid the 1.1 ms the identifier set cost to
  collect, so the front end is now below its pre-campaign baseline on every phase.

  **It changes module semantics**, which is why it landed pre-1.0: a compile error in an unused sibling
  file no longer fails the build, and neither does a conformance that was arriving only because the whole
  directory loaded.

  **Five claims did not survive being run.** Three were caught by the Step-1 measurement and two by the
  implementation; each is recorded because each was asserted rather than measured.

  1. ~~"resolution-level pruning reaches most of the 20 dead units"~~ — the brief settled on 17 units
     remaining, reasoning that a named-but-dead generic TU can never be dropped. True, but it under-counted
     what the closure drops elsewhere: the answer is **10**.
  2. ~~"the intra-directory import graph is sparse, so `import` edges suffice"~~ — the *import* graph is
     sparse but it is not the closure. `priority_queue.kama` has **no `import` at all** and declares
     `DynamicArray<T, A> data;`: an unqualified name resolves against the file's own module scope
     program-wide, so the files of one module reference each other implicitly. The closure follows every
     identifier spelling instead, which is a superset of the references and cannot under-compute.
  3. ~~"parsing is the cheap part, so indexing the directory is free"~~ — at `-O0` parse was 54 % of the
     front end and the objection was real; lever 7 dissolved it. Indexing by **full parse** was the right
     call, and a lightweight declaration scanner would have been wrong for a second reason the brief did
     not have: `view.kama` declares `type view View<T>`, so the type-kind word is a bare identifier rather
     than a closed `value|resource|contract` set. A regex would have missed it.
  4. ~~"a nameless declaration is inert"~~ — two kinds are not, and both are invisible to any closure.
     `type intrinsic <int32> implements Parseable` registers a conformance for a *primitive* under no name.
     Worse, `spawn` and `parallel_for` require `extern "kama_isolate.h";` from
     `lib/std/concurrent/concurrent.kama` while naming nothing in it — and the demand is program-wide, so
     the `spawn` need not even be in the file that did the import. Both providers are marked unprunable.
  5. ~~"an `extern fn` is a declaration like any other"~~ — this one cost more than half the win.
     `extern fn` declares a C symbol, not a module definition, so several files legitimately repeat it:
     three of collections' fourteen each declare their own `extern fn memset`. A reference to `memset`
     from `fixed_array.kama` was dragging in `map.kama` and `bit_set.kama`, and `hasher.kama` behind
     `map`. A name a file declares *itself* is satisfied there and pulls in no sibling — which took httpd
     from 19 units to 10.

  **Residual, not scheduled.** Five dead-but-kept TUs need whole-program reachability rather than
  resolution-level pruning (`dynamic_array`, `fixed_array`, `view`, `ptr`, `stream` are near-empty because
  a generic materializes at its instantiation site, yet their symbols are genuinely imported). They are the
  cheapest population — one definition each — so the remaining prize is small. A bare `import a::b;` also
  loads the whole module by design: nothing pins a file, and seeding from the importing file's tokens would
  be unsound, since a type reached only through inference is never spelled.

<a id="s10"></a>

## 10. Tooling / distribution (deferred)

- **tree-sitter accepts 78 of kama's 84 reserved words as a binding name; the compiler accepts 6 (the contextual ones).**
  Measured 2026-09-02 across the full keyword table, in both type positions:

  ```kama
  Thing else = Thing.make();   // tree-sitter: a clean declaration. kama: parse error.
  isize break = 1;             // same, after a builtin type
  ```

  The two grammars reserve differently *by construction*, which is why this is a gap and not a typo.
  `kama.l` consults one table at **every** identifier, so a reserved spelling is refused everywhere.
  tree-sitter has `word: $.identifier` and extracts keywords **contextually** — a keyword literal is
  only recognised in states where the grammar expects it, and a binding site expects `$.identifier`,
  so every reserved word lexes as a name there. The two that agree, `slot` and `type`, agree because
  the *compiler* accepts them: they are the contextual pair.

  What it costs: every editor on this grammar (Zed, Helix, nvim-treesitter) renders
  `Thing else = …` as a valid declaration with `else` coloured as a variable, and the compiler then
  rejects it — the editor is confidently wrong exactly where a beginner is most likely to be.

  ⚠️ **`tools/check-treesitter.sh` cannot see this class, and one fixture hides it.** Oracle 5 asks the
  compiler only where the two disagree *about files already in the manifest*; detecting "kama raises a
  parse error but tree-sitter is clean" for the whole corpus needs a compiler run per file (~43 s), which
  the guard deliberately avoids. And `tests/xfail/reserved_word_as_name.kama` is in
  `test/parse-errors.txt` and passes — but its ERROR node is at the **use** site
  (`return cast<int32>(base);`), not the declaration. Delete that second line and the entry fails.
  Measured; it is the reason this was found at all.

  **Unprobed, and that is the first task, not the fix:** tree-sitter's keyword extraction has no "reserved everywhere" switch, so the
  fix is either an external scanner (`src/scanner.c` — the grammar has none today, and the `>>` note
  in its header records not needing one as a virtue) or a negative lookahead over 78 spellings baked
  into the `identifier` token. Which of those is tolerable is the question to answer first.


- **AI/agent tooling — SHIPPED.** `kama query --search NAME` / `--diagnostics` / `--json`, the
  `kama agents` command, and the `AGENTS.md` it writes into a project. Record: [agents.md](agents.md);
  guarded by `tools/check-agents.sh`. Residuals, none blocking:
  - **No name-based entry beyond `--search`.** There is no call hierarchy (`callers-of`) and no type
    hierarchy (`implementors-of`), though the contract rename group already holds the data.
  - **No stdin / unsaved-buffer mode.** Every query reads the file from disk, so an agent cannot ask
    about an edit it has not written out. The LSP can; the CLI deliberately cannot.
  - **One full `analyze()` per invocation** — the §9 front-end cache is the fix, and a batch mode the
    cheaper rung.

- **No `scripts` table in `kama.json`.** *(Decided; not scheduled — recorded so it stops being
  re-proposed.)* npm needed one because npm had no build system. kama has `kama build` plus `select`,
  `flags` and `TARGET` entries, so a named configuration is already declarative and portable, which a
  table of shell strings is not. It would also become a second, per-project, undocumented build system
  that every consumer has to read to learn what `test` means. If tasks are ever wanted, the 2.0
  scripting runtime (§7) is the vehicle — a task written in kama, not a shell string — and that is a
  reason to spend the design budget there rather than here.

- **`kama fmt` — a native formatter, not an external tool.** *(Unscheduled.)* The language should print
  itself: one canonical form, applied by the toolchain, so a project never argues about style and a diff
  never carries noise that is not a change. Driven by the project's `kama.json` (the same manifest that
  already carries the flag universe and the toolchain pin), with a small, deliberately non-negotiable set
  of knobs — indent width, line width, brace style — rather than a style language. `kama fmt --check` is
  one more `tools/check-*.sh`, glob-enrolled automatically.

  **This entry used to be scheduled with mandatory braces, and used to argue tree-sitter was the cheap
  substrate. Both were wrong; the corrections were measured, and are recorded here so they are not
  re-derived.**

  - **It was never the migration tool.** The brace rule's entire migration was **2 sites in 1 file** —
    2,116 bodies were already braced and there were zero same-line bare bodies. Braces shipped alone.
  - **Do NOT link the vendored tree-sitter parser.** `tree-sitter-kama/src/parser.c` is a generated
    *table* — the only `ts_*` symbols it defines are `ts_lex`/`ts_lex_keywords`. The parsing engine is
    tree-sitter's **runtime library**, which this repo does not vendor (three headers only; `node_modules/`
    is gitignored). Linking it means vendoring a third-party C runtime into `src/` permanently *and*
    making the editor grammar load-bearing for the compiler. The old "the CST is already trustworthy"
    argument is true and still does not reach that conclusion.
  - **Use the compiler's own front end.** Comments die in exactly three lexer rules (`kama.l`: the
    `{reserved_preprocessor}` and `{single_line_comment}` actions, and the `IN_COMMENT` start state).
    Retaining them is a `vector<Trivia>` on `LexerInstanceData` — the same struct that already carries
    `identTokens`, an `unordered_set<std::string>` taking *one insert per identifier token on a ~14 ms
    parse*. At ~8k comments corpus-wide the cost is below measurement noise, and a `bool keepTrivia`
    gates it to the fmt path anyway. This is also the only route that keeps one parser, and it serves the
    LSP.
  - **Byte offsets are not needed** — attaching a comment requires only a *monotonic* position, and
    `(line, col)` already is one. (An earlier brief claimed the opposite and would have sent the work
    through a much larger lexer change.)
  - **The cost is the printer, not the parser.** 72 AST node types; the C emitter needed ~530 dispatch
    arms over them. `CEmitter::unparseExpr` (`kama.cemit.cpp`) is a partial precedent — it renders
    expressions back to kama text for `assert` messages — but it covers ~20 node types, is deliberately
    lossy (`return ""` on anything unhandled), and drops literal spelling (`'a'` → `97`), so it is proof
    of shape, not a foundation. Retaining raw literal spellings is a prerequisite either way.
  - **Policy calls to make against a working printer, not in the abstract.** The corpus already agrees on
    4-space indent (zero tabs), operator/comma spacing, no paren padding, no trailing commas, and ≤1
    consecutive blank line — a formatter enforcing only those is a near no-op. What it would *change* is
    the contested part: **6,090** one-line `{ … }` blocks, **1,586** one-line `fn` bodies, **3,486**
    hand-aligned lines, a de-facto line width of **110** (picking 100 rewraps ~14.5% of the corpus), and
    brace style — `lib/`, `prelude/` and `examples/` are 100% K&R while `tests/`, `bench/` and, most
    visibly, both `seed/` templates carry Allman.
  - **Free leverage:** `tools/check-treesitter.sh` already maintains a 1,000+ file corpus both parsers
    agree on — a ready-made test set for the two properties that matter, **idempotence**
    (`fmt(fmt(x)) == fmt(x)`) and **semantic preservation** (reparse, or compare emitted C).

- **C symbol naming — folded into the module-system campaign, and SHIPPED in it** (`0.9.81`–`0.9.84`).
  Both defects were downstream of a model with two ways to name a thing, which is why they were folded in
  rather than fixed where they showed. A kama identifier that is a C keyword is now REFUSED at the lexer —
  kama reserves the whole C11 + C23 set — rather than emitted raw for clang to choke on; and a generated
  `.c` is named by its module while a file-private symbol is named by its file, with units emitted in a
  canonical order, so `--keep-c` is reproducible. The campaign's design doc was deleted when it closed, per this
  file's own maintenance rule; the record is the git log, SPEC's *Modules* section, and `docs/packages.md`.

- **Devirtualize a contract-value call in DEBUG builds.** `Comparable<int32> c = l; c.compareTo(other: r);`
  costs nothing at `-O2` — clang folds the `static const` vtable pointer, devirtualizes, inlines the thunk,
  and reduces the whole call to six instructions with no call at all. At `-O0` the indirection survives, so
  a debug build pays for a fat pointer whose target the compiler knew when it wrote it one line earlier.
  kama already does the analogous analysis for inheritance — `buildVtables` keeps a whole-program override
  index so a never-overridden slot lowers to a direct call ([kama.cemit.cpp](../src/kama.cemit.cpp), guarded by
  `tools/check-ecs-zero-dispatch.sh`). The contract-value case needs less: a local "this fat pointer's
  `vtbl` was assigned a known constant and never reassigned" check. Low priority — release builds are
  already optimal, and this only buys debug-build speed.

- **Build configuration + cross-compilation — residuals.** The target/build-type/output selection model is
  done ([targets.md](targets.md), [SPEC.md](SPEC.md)). What is left:
  - **⚠️ No CPU-tuning knob.** kama passes **no** `-march`/`-mcpu`/`-mtune` anywhere, so every build targets
    the architecture's *generic baseline*. That is the right default (portable binaries — and it is why
    `zig cc` and clang measure identical, neither tunes), but there is no first-class way to say otherwise:
    the only route today is `"cflags": ["-mcpu=…"]` on a declared `select.TARGET` entry, so **a plain
    `kama build --release` cannot tune for the host at all**. Every peer has a shorthand (Rust
    `-C target-cpu=native`, Zig `-mcpu=native`, gcc/clang `-march=native`). Likely shape: a `cpu` field on a
    target spec, plus a `native` spelling for host builds. Wants a before/after benchmark first — kama's
    emitted C is fairly generic, so the win may be small outside float/SIMD-heavy code.
  - **Per-value `BUILD_TYPE` settings** (own opt-level/LTO/strip), deliberately deferred so `kama.json` does
    not become a build-settings language; and **numeric build options surfaced as `comptime` constants**
    rather than as flag comparisons (`@compileFor` stays tagging, not logic).
- **VS Code Marketplace publish** — the `.vsix` is built and attached to releases; Marketplace publishing is
  deferred, and gates on a public release.
- **Language server (LSP) — residuals.** `kama lsp` and its eight editors are documented in
  [editors.md](editors.md). Forward work:
  - **Tree-sitter grammar + Zed extension — residuals.** `tree-sitter-kama/` is guarded by
    `tools/check-treesitter.sh`; `editor/zed/` is the Zed extension. Forward work:
    - **Flip the grammar source to the public URL** when the repo goes public — a tag `rev` +
      `https://github.com/cosmic-canopy/kama` in `editor/zed/extension.toml`, and the `git`+`subpath` form
      in the Helix snippet. Both spellings are already written out in `docs/editors.md`; this is a
      two-line change gated purely on visibility.
    - **Close the Zed loop.** The Rust component is compile-verified against `zed_extension_api` and the
      queries are checked, but `zed: install dev extension` is a GUI action and has not been run.
    - **Registry/ecosystem registrations, all gated on the repo being public:** publish to the Zed
      extension registry; nvim-treesitter `install_info` with `location = 'tree-sitter-kama'`; the GitHub
      linguist PR (`provisioning/linguist/languages.yml.snippet` still points at the TextMate grammar);
      and upstreaming the Helix `[[language]]`/`[[grammar]]` entries — the same class of work as the
      `nvim-lspconfig`/`eglot-server-programs` registrations below.
  - **The fixed prelude-ANALYSIS floor per keystroke** — see *Cache the toolchain front end* in §9, which
    is the same defect measured on the build path and is where the fix belongs. M5 removed the prelude
    *parse* from every keystroke; analyzing it again on every buffer change is what remains, and it is a
    floor no file can get under. ⚠️ **Re-measure before acting on it.** The ~85 ms/keystroke figure this
    was sized against predates §9 lever 7, and `analyze` alone got 5.4× faster when the compiler started
    being built optimized; **closure pruning then took `analyze` a further 46 % on httpd** (26.5 → 14.2 ms)
    by shrinking what a keystroke has to analyze at all. Two large cuts have landed under this number
    since it was taken, so whether the floor is still worth a campaign is an open question, not a settled
    one — and note the *prelude* share is the part neither cut touches.
  - **One build configuration per server process.** It is pinned by the first opened document that resolves
    a manifest, so in a monorepo whose packages declare *different* flag universes the unpinned packages get
    the pinned one's configuration. Softened, not fixed: the status bar says which is active and
    `kama.restartServer` exists. The real fix needs **per-configuration parse caches**, because
    `pruneInactiveDecls` rewrites cached units in place.
  - **Upstream editor registration** — a `kama` entry in `nvim-lspconfig`, in Helix's built-in
    `languages.toml` and in `eglot-server-programs`. Turns six pasted lines into zero for users, but these
    are PRs to *other* projects and gate on a public release.
  - **Three editor snippets are documented but unverified** — Vim (coc.nvim), Sublime Text and Kate. Each
    needs a human at a GUI; Sublime additionally needs its LSP package installed through Package Control.
- **Workspace-internal dependencies — one follow-on.** Workspaces work today ([packages.md](packages.md)).
  What is left: version reconciliation on publish — `kama publish` substituting a registry version for a
  workspace path dep.
- **kama-aware debugger value formatting — polish on the working debugger.** Breakpoints/stepping are already
  kama-source-level, but inspected values render in their emitted-C form (a `string` shows as
  `kama_string {data,len,cap}`, `Optional<T>` as its tagged union, collections as C structs). Add LLDB type
  summaries / synthetic providers (CodeLLDB supports Python formatters) so `string`/`Optional`/`Result`/the
  collections/smart-pointers render as kama values. Small next to the LSP, high polish-value, builds directly
  on the shipped debug flow.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
- **Package manager (ecosystem foundation).** A first-class dependency manager + registry so libraries distribute
  without vendoring — the point at which cross-package conformance coherence (SPEC § *`type intrinsic`*) becomes load-bearing.
  User docs (including the registry protocol a host must serve): [packages.md](packages.md). What remains is
  hosted-services and ops work:
  - **Official vs community packages — DECIDED and SHIPPED (2026-09-06).** The `@kama` scope is the mark
    and the channel (`packages.md` § Scopes says so); `@kama`/`@std` get reserved the day M3.3's host
    exists. **`@kama/sodium` shipped** as `../kama-sodium` — libsodium 1.0.20 vendored through
    `csources`/`cincludes`, six modules, proven native (debug/release), wasm and through a file-registry
    publish→install round trip. What it forced in-tree: `kama seed --license`, the `cincludes` key,
    the move-only propagation fix, `csources` as gnu11, `std::digest`; what it rowed: the four rows above
    NOW and the member-visibility question in §3. Its AGENTS.md § "This package" is the first draft of
    the library-kind guidance addendum.

      **Decided direction:** follow where the ecosystem landed rather than per-developer signing keys.
      Go ships no package signatures at all and leans on the `sum.golang.org` transparency log; PyPI
      *removed* PGP in 2023 (almost nobody verified) and replaced it with OIDC Trusted Publishing +
      attestations; npm did the same via sigstore provenance; crates.io ships checksums only. So:
      **(a)** near-term, an allowed-signers set — real `ssh-keygen -Y verify` against a configured trust
      set, plus closing the warm-store and git-dep gaps; **(b)** then CI/OIDC provenance recorded in a
      transparency log, at which point verification becomes mandatory. Both gate on a live registry, since
      mandatory verification is meaningless before one exists.
- **Longer-term — a "node.js-class" application framework in kama.** A fast, low-overhead server/app framework
  (HTTP already dogfooded via `examples/httpd`), aiming to beat the Node/Deno overhead profile on the no-GC/AOT
  (or VM-scripted) runtime — the flagship *application* of the language + package manager + scripting tiers
  together. See [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md). Aspirational, post-ecosystem.
