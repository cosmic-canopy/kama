# M2 — stdlib parity campaign (cold-start brief)

*In-flight campaign doc. **Delete this file when M2 ships**, once SPEC + the module docs carry the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

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
| `str::parse::<T>()` | ✗ nothing anywhere | M2a |
| `slice::sort`, `binary_search` | ✗ nothing | M2a |
| `f32`/`f64` math: trig, `powf`, `floor`, `ceil`, `round`, `exp`, `ln` | partial — float32 only; **trig is `extern`-declared but not exported** | M2a |
| `char::is_alphabetic` / `is_numeric` / … | ✗ nothing | M2a |
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

Each is one session ending at a commit. Order matters only in that M2a's `Comparable`-generic work informs
M3's naming reconcile.

- **M2a — language-adjacent primitives.** parse · sort/binarySearch · `std::math` completion · `char`
  classification.
- **M2b — the OS surface.** `std::fs` completion + path helpers · `std::io` stream handles + `lines()` ·
  `std::time` sleep + wall clock · DNS.
- **M2c — the two new modules.** `std::random` · `std::encoding`.

## Decisions

**1-3 are DECIDED (user, 2026-08-01) — build to them, do not re-open.** 4-7 carry a lean; confirm or
overrule at session start rather than discovering it halfway through. The standing constraint is **no
compromises on design**: at 1.0 these names and shapes freeze.

1. ✅ **DECIDED — `sort` lives in `std::collections` as free functions over `View<T>`.** Beside `View`,
   not in a new `std::algorithm`. A `sort` taking a `View<T>` gets `DynamicArray` and `FixedArray` for
   free through their existing `view()`, and `View` is already the "contiguous run of T" abstraction. A
   separate module would be a second place to look for one function.
2. ✅ **DECIDED — `sort` is ALLOCATION-FREE.** Heapsort, or introsort with a fixed-size stack. Not a
   preference: a merge sort's auxiliary `DynamicArray<T>` inside a generic free function hits the tracked
   generic-instantiation limitation (ROADMAP §2), *and* an allocating sort is unusable under
   `@noheap` / `--no-heap` and on MCU — where sorting a fixed buffer is precisely the use case.
   `PriorityQueue`'s `siftDown` is the in-place prior art. Ship `binarySearch` alongside.
3. ✅ **DECIDED — parsing returns `Optional<T>`.** `parseInt` / `parseUint` / `parseFloat`, matching
   `string.find`'s shape. The failure is "this isn't a number", which carries no detail worth a payload;
   `Result<T, ParseError>` invites an error enum nobody reads. Lift the scanner out of `JsonReader`
   (`numToken`/`readInt`/`readUint`, `lib/std/serialization/json/json.kama:291-320` — currently PRIVATE
   methods) so there is one implementation and JSON calls it.
4. **Path helpers: free functions on `string`, or a `Path` type?** *Lean: free functions*
   (`join`/`dirname`/`basename`/`extension`), because a `Path` type means two string-ish types and GOALS #4
   says one way to do a thing. Rust's `Path` earns its keep through OsString encoding concerns kama does not
   have — it is UTF-8 everywhere.
5. **Does `std::io` gaining `stdout()` conflict with the floor's `print`/`println`?** They coexist
   deliberately: the floor's print family is *always available and unbuffered*, for diagnostics that must
   work under `--no-std`; `std::io`'s handles are `Reader`/`Writer` values that *compose* with `pump`,
   `BufWriter` and serde. Say that in FLOOR.md so it does not read as duplication.
6. **`std::random` seeding.** Needs an entropy source, which is a platform seam (`getrandom`/`BCryptGenRandom`/
   `crypto.getRandomValues`). *Lean: a `kama_random.h` seam with an explicit `seed:` ctor as the primary
   API* — a deterministic PRNG you can seed is the useful thing for games and tests; OS entropy is the
   convenience. **Document loudly that it is not cryptographic.**
7. **Does `min`/`max`/`clamp` become generic here or in M3?** It is a *breaking* change (it replaces the ten
   `minI32`… in `lib/std/num/ops.kama:6` and the five `minf`… in `lib/std/math/scalar.kama:5`), so it
   belongs to M3's reconcile — but M2a should not add MORE suffixed spellings in the meantime.

## Mechanics you will want to know

- **A new stdlib module needs no registration anywhere.** `import std::foo::{X}` resolves by path to
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

## kama idioms that will bite you (learned the hard way, written nowhere else)

Every one of these cost a build cycle while writing M1's fixtures:

- **A `match` subject must be a typed local.** `match (give decode::<T>(src))` is rejected — bind
  `Result<T, Owned<Error>> r = …;` first, then `match (give r)`. (Tracked in SPEC § Known limitations.)
- **`slot x` + `ref x` is rejected** — a `ref` is a read borrow of a live value. Use `out`, or restructure
  so the callee returns instead. In `json.kama` the fix was to drop the out-param entirely and ride the
  reader's existing sticky `err` flag.
- **A free function cannot carry `when [T: Copyable]`** — that clause is method-only. Put the bound in the
  type-param list.
- **Interpolation holes take an identifier with `.field`/`[i]` only** — `"${a.length()}"` is a lexical
  error. Bind the call's result to a local first.
- **A `resource`'s fields are always private** — `public int32 v;` on a `type resource` is an error.
- **`@generate` requires every field marked** `@field` or `@skip`.
- **Named args everywhere**, including the ones that read like keywords: `println(s: "…")`,
  `decode::<T>(src: …)`, `FixedArray.make(size: …)`.
- **Generic free functions infer their type args from the call** — `max(a: 3, b: 4)`, no turbofish. The
  explicit form is only for `decode::<T>` shapes where inference has nothing to go on.
- **A `.d/` fixture is a directory of files built together**; a bare `namespace` in a single-file fixture
  will not resolve a sibling.

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
```

Baseline at the end of M1: **native 855 / ASan 821 / wasm 795, all 0 failed.**

Every new function needs a fixture. `std::log` currently has **zero** and `std::time` has **two** — do not
add to that pattern; the module you write is the module you test.

## Explicitly out of scope

IPv6 · UDP multicast · TLS/HTTPS · HTTP · Unix sockets · regex · crypto/checksums · `trySend`/`tryRecv`/
unbounded channels/`select` · the job system + event-loop scheduler · `std::io` compression adapters ·
YAML/XML serde backends · a Unicode module (casing/whitespace stay ASCII) · a `std::gpu` kama wrapper.

All are tracked in [ROADMAP.md](../ROADMAP.md) §1–§2. Rust ships none of them in `std` either, so they are
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

**What genuinely deserves scheduling is `substring`.** It takes a **byte** range and bounds-checks only
against `len`, not against codepoint boundaries — so `"AéZ".substring(start: 0, end: 2)` returns a 2-byte
string ending in a lone `0xC3` lead byte. **That is not valid UTF-8, produced from valid input, in the
safe surface, with no `unsafe` and no error** — the one place kama can break its own string invariant.
Recommended fix, consistent with the language's existing discipline (indexing traps rather than invoking
UB, and `Optional` is for absence rather than for programmer error): **trap on a non-boundary offset**,
exactly as it already traps out-of-range, and exactly as Rust's `&s[0..2]` panics on a non-char-boundary.
The check is O(1) — a byte at a boundary must not be a continuation byte (`(b & 0xC0) != 0x80`). Decide
this at session start; it is a one-line runtime change plus a `tests/trap/` fixture.
