# M2 — stdlib parity campaign (cold-start brief)

*In-flight campaign doc. **Delete this file when M2 ships**, once SPEC + the module docs carry the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> ### ►► M2a is SHIPPED. M2b and M2c remain — read this first
>
> **M2a (parse · sort · `std::math` completion · `char` classification) landed 2026-08-04.** The record is
> in SPEC (§ *Sorting & searching*, § *Parsing*, § *ASCII*, § *Math*); the three spikes below are resolved
> and their answers are summarised here so M2b/M2c inherit them. Baseline is now **native 927** (was 915).
>
> **The spikes, as decided:**
> - **A — sort API.** Free functions over `View<T>` in `std::collections`, never per-container methods, so
>   one implementation covers `DynamicArray`/`FixedArray`/sub-ranges. `View<T>` gained `swap`+`reverse`;
>   that was the right call, not a hack around, because a view is second-class in *escape*, not in
>   mutability. Ordering comes from `Comparable` or an `Order<T>` **contract** — an `fnptr` could not
>   express it (no generic fn-pointer types, now ROADMAP_DETAIL §2) and would have been the worse tool regardless:
>   an indirect uninlinable call, and stateless in a language with no closures.
> - **B — stability.** Both, and the stable one sorts an **index permutation** rather than a scratch buffer
>   of `T` — that keeps it O(n log n) *and* free of a `Copyable` bound. `sort`/`sortWith` are
>   `@compileFor(!NOHEAP)` so a no-heap build cannot silently allocate.
> - **C — parse.** `Result<T, ParseError>` with `Empty`/`InvalidDigit`/`OutOfRange`, reached generically as
>   `parse::<int32>(s:)` via a marker contract + fallible-ctor `type intrinsic` impls (the serde pattern). No suffixed
>   `parseI32` ladder, so M3's naming reconcile is unaffected.
> - **D (unplanned) — float64 math.** `std::math`'s scalar surface became **one generic function per
>   operation over a `Real` contract**, so `sqrt(x: 1.0)` and `sqrt(x: 1.0f32)` are the same name with the
>   width inferred. **Not breaking** — an earlier draft took C's `sqrt`/`sqrtf` split and was reverted. 21
>   names instead of 42, and `Real` is exported so a user type can join in. The only removal is `absf`,
>   which was exported but never called anywhere; generic `abs` replaces it.
>
> **Five compiler defects surfaced and were fixed** — four of them pre-existing on `dev`, none specific to
> this campaign:
> 1. **A generic free function calling another with a forwarded type parameter CRASHED the compiler.**
>    Turbofish type args were stored without substitution, so `T` bound to itself and `mangleElem` recursed
>    until the stack died. No code in the tree used the shape, so it had never been hit. Fixed by
>    substituting + deferring an unbound arg, and by re-walking generic *function* bodies once per
>    instantiation (generic *types* already had that pass). Regression fixture: `tests/generic_fn_forward`.
> 2. **Generic inference could not bind `T` from a `View<T>` argument** — only a *bare* `T x` parameter was
>    inferable, which is why `tickAll::<Timer>` in the ECS fixture carried a turbofish. Now unifies type
>    arguments positionally.
> 3. **A generic's contract bounds resolved in the CALLER's namespace**, so `parse<T: Parseable>` demanded
>    every caller import a marker contract they never name. Bounds now resolve in the template's home scope.
> 4. **`exprTypeNode` could not type a negated literal or a non-generic call result**, so `abs(x: -5.0)`
>    and `log(x: exp(x: 1.0))` failed to infer once math went generic. Unary-minus and non-generic call
>    returns now resolve; a nested *generic* call still needs a bound local (its return type is the `T`
>    being inferred).
> 5. **Release packaging staged only two of ten seam headers** — `kama_fmt/time/log/app/ctrl/channel/
>    isolate/atomic.h` were all missing from the payload. Now globbed.
>
> ⚠️ **A broken file anywhere in a directory module breaks every program importing that module** — while
> `sort.kama` was mid-development it took down every `std::collections` importer. Expect that when adding a
> file to an existing module directory.
>
> ⚠️ **`Order<Owned<T>>` is not instantiable**: a `ref` parameter may not name a smart pointer. Sort a
> container of the resources themselves.
>
> ✅ **The conformance mechanism M2a leaned on has since been replaced.** `Real`, `Parseable` and
> `ParseableRadix` exist as contracts only because an intrinsic had no way to declare conformance; it now
> has one (`type intrinsic <…> implements C`), and the retroactive block is gone — the contract-model
> campaign is complete, and what the language now *is* lives in [SPEC.md](../SPEC.md).

## Why this campaign exists

kama's language surface is complete and the correctness pass (M1) is done. What is left before 1.0 is that
**the standard library cannot do several things a new user hits in their first afternoon** — parse a string
to a number, sort an array, call `sin`, sleep. The bar the user set is *production quality, and at least
parity with the major languages*.

**The comparator is Rust's `std`**, and the choice matters. It is the only peer that is also no-GC, and the
only one whose stdlib deliberately stops before regex / TLS / HTTP / crypto and pushes those to packages.
kama now *has* a package manager, so that is the right line to draw. Go / Python / C# ship bigger batteries
because they ship a runtime alongside them; matching *those* is an ecosystem goal, not a 1.0 goal.

**kama already meets or beats Rust `std`** on: collections (broader — `SlotMap`, `BitSet`,
`SortedMap`/`SortedSet`, `Deque`, `PriorityQueue`, pluggable `Hasher` + `Allocator` on every container),
smart pointers, concurrency primitives, `process`, `env`/`args`, string formatting and interpolation. And it
ships three things Rust `std` does not: JSON + binary serialization, `std::log`, and the `fmt` tag functions.

## The gap — this table IS the campaign

| Rust `std` | kama today | Lands in |
|---|---|---|
| `str::parse::<T>()` | ✅ **shipped** — `std::fmt::parse::<T>` | M2a |
| `slice::sort`, `binary_search` | ✅ **shipped** — `std::collections` free fns over `View<T>` | M2a |
| `f32`/`f64` math: trig, `powf`, `floor`, `ceil`, `round`, `exp`, `ln` | ✅ **shipped** — both widths, C's names | M2a |
| `char::is_alphabetic` / `is_numeric` / … | ✅ **shipped** — `std::ascii` | M2a |
| `thread::sleep` | ✗ — `kama_sleep_ms` exists in `kama_os.h`, reachable only from a test helper | M2b |
| `SystemTime` (wall clock / UNIX epoch) | ✗ — `std::time` is monotonic-only | M2b |
| `std::path` + `fs::create_dir` / `rename` / `metadata` | ✗ no path helpers, no `mkdir`; `Metadata` is `{size, isDir}` | M2b |
| `io::stdin/stdout/stderr`, `BufRead::lines` | ✗ no stream handles, no `lines()` | M2b |
| `ToSocketAddrs` (DNS) | ✗ numeric hosts only | M2b |
| `sync::{Mutex, RwLock, Once}` | **by design** — shared-nothing model; `Atomic<T>` is the one shared-mutable seam | — |

Two modules go **past** Rust `std`, toward the Go/Python battery, because they are cheap and constantly
wanted: **`std::random`** and **`std::encoding`** (base64 + hex). Both land in M2c.

`sync::{Mutex, RwLock, Once}` is the one row that is a **stance, not a gap** — say so in the docs rather
than leaving a reader to wonder.

## Session split

**The prerequisite language batch is DONE** — shipped ahead of M2a so these fixtures are not written
against a surface about to shift. `match` now takes a call subject (plain enums included), a static on a
generic type is reachable as `Type::<args>::name()`, the dot-on-type diagnostics say what the name
actually is, and no message leaks a mangled name. See SPEC § *Strings* / § *Known limitations* and the
git log; nothing in the parity gap below depended on any of it.

Each of the three below is one session ending at a commit. Order otherwise matters only in that M2a's
`Comparable`-generic work informs M3's naming reconcile.

- **M2a — language-adjacent primitives.** parse · sort/binarySearch · `std::math` completion · `char`
  classification.
- **M2b — the OS surface.** `std::fs` completion + path helpers · `std::io` stream handles + `lines()` ·
  `std::time` sleep + wall clock · DNS.
- **M2c — the two new modules.** `std::random` · `std::encoding`.

## Open questions

**Spikes A, B and C are RESOLVED and M2a is shipped** — see the banner at the top of this file for what was
decided and why, and SPEC for the surface itself. What follows is what M2b/M2c still have to answer.

### Still open — M2b / M2c (leans only)

4. **Path helpers: free functions on `string`, or a `Path` type?** *Lean: free functions*
   (`join`/`dirname`/`basename`/`extension`) — a `Path` type means two string-ish types, against GOALS #4.
   Rust's `Path` earns its keep through `OsString` encoding concerns kama does not have (UTF-8 everywhere).
5. **Does `std::io` gaining `stdout()` conflict with the floor's `print`/`println`?** They coexist
   deliberately: the floor's print family is always-available and unbuffered, for diagnostics that must work
   under `--no-std`; `std::io`'s handles are `Reader`/`Writer` values that COMPOSE with `pump`, `BufWriter`
   and serde. Say so in FLOOR.md so it does not read as duplication.
6. **`std::random` seeding.** Entropy is a platform seam (`getrandom` / `BCryptGenRandom` /
   `crypto.getRandomValues`). *Lean: a `kama_random.h` seam, with an explicit `seed:` ctor as the PRIMARY
   API* — a deterministic, seedable PRNG is what games and tests actually want; OS entropy is the
   convenience. **Document loudly that it is not cryptographic.**
7. **Does `min`/`max`/`clamp` become generic here or in M3?** Breaking (it replaces the ten `minI32`… in
   `lib/std/num/ops.kama:6` and five `minf`… in `lib/std/math/scalar.kama:5`), so it belongs to M3's
   reconcile — but M2a must not add MORE suffixed spellings meanwhile.

## Mechanics you will want to know

- **A new stdlib module needs no registration anywhere.** `import { std::foo::X };` resolves by path to
  `lib/std/foo/foo.kama` (or a directory of files sharing `namespace std::foo;`). Release packaging is
  `cp -R lib/std` (`.github/workflows/release.yml`), so a new directory ships automatically.
- **A module's cost is its seam header.** `extern "<math.h>"` is what makes the driver link `-lm`; the same
  pay-for-what-you-use rule is why `Atomic` sits behind `std::concurrent`. Anything needing a new C seam
  gets its own `kama_*.h` next to the module.
- **Floor vs `std::`** is decided by one rule, in [FLOOR.md](../FLOOR.md) § "What is floor, and what is an
  `import`": contracts/syntax/intrinsics are always on; backends and concrete implementations are opt-in.
  Nothing in M2 belongs in the floor.
- **`--no-std` and `--target embedded` must keep working.** M2 only adds to `lib/std`, so it should be free —
  but `tools/check-noheap.sh` and `tools/check-embedded.sh` are the proof, not the assumption.

## Where kama differs from what an LLM will reach for

**Moved to [../coming-from-other-languages.md](../coming-from-other-languages.md)**, and linked from
`llms.txt`. It was drafted here, but this file is deleted when M2 ships — LLM-facing guidance cannot live
in a doc with an expiry date. Its snippets are now compiled by `tests/idioms_kama_way.kama`, so they cannot
rot silently.

Read it before writing M2 fixtures; the one-line summary is that **kama's inference works from bound
locals**, so a match subject, an interpolation hole and a generic argument each want an intermediate name.

## Verification

M1 added two invariants that M2 must not break — both are cheap and both caught real bugs:

- **`kama check` ≡ `kama build`** across the corpus, with **zero declared exceptions**. If a new stdlib
  function makes the analysis path disagree with the build path, the suite says so by name.
- **Builds are warning-free.** A fixture that compiles *with* a C-compiler warning is a failure. Adding an
  FFI seam is exactly where this earns its keep (M1 found a `void**`/`uint8_t**` mismatch that way).

```sh
tools/cdev make && tools/cdev test                     # native: fixtures, xfail, agreement, warnings
tools/cdev exec env KAMA_SAN=1 ./run_tests.sh          # ASan + UBSan
tools/cdev exec env KAMA_WASM=1 ./run_tests.sh         # wasm (net/process auto-skip)
tools/cdev exec sh tools/check-noheap.sh               # nothing new reaches the heap unbidden
tools/cdev exec sh tools/check-embedded.sh             # freestanding build still links
sh tools/check-treesitter.sh                          # every new fixture must PARSE — run on the HOST
```

⚠️ **`check-treesitter.sh` SKIPs when no tree-sitter CLI is present**, which is always the case inside the
container — that is how it sat red on `dev` through a whole campaign. Run `npm ci` in `tree-sitter-kama`
once, then run the script on the **host**. Note `./kama` is a symlink to whichever platform built LAST, so
a `tools/cdev make` leaves the host script pointing at a Linux binary.

Baseline entering M2a: **native 915 / ASan 879 / wasm 853, all 0 failed** (re-measured 2026-08-04 after
the inheritance campaign; the 867/832/806 this file was written with is stale, as was 855/821/795 before it).

One harness invariant was added with that batch and is worth knowing before you write fixtures: an `xfail`
must be rejected **cleanly**. A compiler that dies by signal used to satisfy "did not build" and scored a
PASS, which is how three segfaults sat green — both the xfail leg and the analysis-agreement leg now fail
on a signal.

Every new function needs a fixture. `std::log` currently has **zero** and `std::time` has **two** — do not
add to that pattern; the module you write is the module you test.

## Explicitly out of scope

IPv6 · UDP multicast · TLS/HTTPS · HTTP · Unix sockets · regex · crypto/checksums · `trySend`/`tryRecv`/
unbounded channels/`select` · the job system + event-loop scheduler · `std::io` compression adapters ·
YAML/XML serde backends · a Unicode module (casing/whitespace stay ASCII) · a `std::gpu` kama wrapper.

All are tracked in [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md) §1–§2. Rust ships none of them in `std` either, so they are
a post-1.0 *package ecosystem* story.

### On Unicode — and the one thing that IS worth scheduling

**ASCII-only casing and whitespace do not violate UTF-8-everywhere, and are not a stdlib gap.**
[utf8everywhere.org](https://utf8everywhere.org) is a doctrine about **encoding**: one internal
representation, UTF-8, never UTF-16/`wchar_t`, no conversion at internal boundaries. kama satisfies that
completely — one `string` type, byte `length()`, `s[i]` a `uint8`, `.chars()` the explicit opt-in for
codepoints, and `foreach (char c in s)` a deliberate type error. Case mapping is a different axis
(*Unicode semantics*), and utf8everywhere actively argues against casual per-character operations, pointing
at a real library when you need them.

It is also **safe**: `toLower`/`toUpper` leave bytes ≥ 0x80 untouched, and `is_ws` matches only bytes that
can never appear inside a multibyte sequence, so casing and trimming cannot split or corrupt a codepoint.
The observable limit is that `"Ä".toLower()` is unchanged and U+00A0 is not trimmed.

**Zig — the closest peer (no-GC, AOT, and kama's own bundled backend) — does exactly the same**:
`std.ascii.toLower` is ASCII, and full Unicode casing is a package. Rust/Go/Python/C#/JS bake Unicode
tables into their stdlib, but they are all either GC'd or indifferent to binary size; kama targets MCUs
with `--no-heap`. So this is a **considered non-goal for `std`** and belongs in a package. *(An earlier
note in this file called it "behind every peer" — that was wrong, and is corrected here.)*

*(The `substring` hole that this section used to schedule has SHIPPED — it traps on a split codepoint,
with `floorCharBoundary`/`truncate` as the total operations. SPEC § *Strings*.)*