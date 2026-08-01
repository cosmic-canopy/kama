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

**Prerequisite, before M2a: the generic-static spelling** (`Type::<args>::name()`) and the three dot-on-type
diagnostic defects — decided, and written up in [ROADMAP.md](../ROADMAP.md) §2. It is a LANGUAGE change, so
it lands before M2's fixtures are written against a surface that is about to shift. Nothing in the parity
gap depends on it; the ordering is only to avoid churning fixtures.

Each of the three below is one session ending at a commit. Order otherwise matters only in that M2a's
`Comparable`-generic work informs M3's naming reconcile.

- **M2a — language-adjacent primitives.** parse · sort/binarySearch · `std::math` completion · `char`
  classification.
- **M2b — the OS surface.** `std::fs` completion + path helpers · `std::io` stream handles + `lines()` ·
  `std::time` sleep + wall clock · DNS.
- **M2c — the two new modules.** `std::random` · `std::encoding`.

## Open questions — THREE SPIKES + ONE BUG FIX, before any M2a code

None of these is a default to proceed on. Each is a real design decision that freezes at 1.0, and the user
has asked for the *professional-grade* answer, not the expedient one. **Run the spikes first; they are
research + a written recommendation, not implementation.**

### Spike A — the `sort` API (blocks M2a)

Two entangled questions: **where it lives**, and **what it can sort**.

*Survey properly, don't guess.* At minimum: Rust (`slice::sort` / `sort_unstable` / `sort_by_key`, reached
through deref so `v.sort()` works), Go (`sort.Slice` / `slices.Sort` — note the generics rewrite in 1.21),
C++ (`std::sort` / `ranges::sort` over iterators), Zig (`std.mem.sort` — closest peer, takes a slice + a
comparator fn), Swift (`Array.sorted()` / `sort()` on `MutableCollection`), C#/Java. The question to answer
is not "what is popular" but **what shape fits a language with `View<T>` as a first-class stack-only borrow
and `Comparable` as a contract**.

Specific things the spike must resolve:

- **Free function over `View<T>`, method on the containers, or both?** A free `sort(items: View<T>)` gets
  `DynamicArray`, `FixedArray` and sub-ranges (`slice`) from ONE implementation; `xs.sort()` reads better
  but is per-container and cannot sort a sub-range or a `View` obtained from elsewhere. "Both" is what Rust
  effectively has, at the cost of two spellings (GOALS #4).
- **⚠️ `View<T>` cannot swap elements today.** `DynamicArray.swap` needs a private `takeAt` plus raw
  `Ptr<T>` aliasing, because the move tracker rejects `this.data[i] = …`
  ([dynamic_array.kama:213-222](../../lib/std/collections/dynamic_array.kama#L213)). `View` has a
  place-returning `operator[]` but no `swap`. So a View-based sort either restricts to a copyable element
  or needs a new `View.swap` with the same unsafe internals. **Decide this deliberately — it is the part
  most likely to be hacked around.**
- **How is the ordering supplied?** `T: Comparable` (kama's contract, prelude retro-impls on every
  primitive) is the obvious default. Do we also want a `sortBy(items:, less:)` taking an `fnptr`, given
  kama has no capturing closures (WEB_FRAMEWORK_READINESS Tier-1)? Without it, sorting by a computed key
  means a wrapper type.
- **Stability.** See below — it is a free choice, not a forced one.

### Spike B — stability, and what `sort` guarantees

**Correction, and it changes this question.** This brief previously said a stable merge sort was
*unimplementable* because a generic free function could not allocate a `DynamicArray<T>` scratch buffer of
its own type param. **That is false, and was verified false:**

```kama
fn int32 mergeScratch<T>(View<T> items) {
    DynamicArray<T> scratch = DynamicArray.withCapacity(capacity: items.length());   // builds and runs
    return scratch.length();
}
```

The ROADMAP §2 entry claiming otherwise was stale and has been corrected. (What IS still broken is narrower
and unrelated: a `static fn` on a GENERIC type has no spelling that reaches it — see ROADMAP §2. It affects
nothing in M2.) So stability is a genuine trade-off with both options available, not a capability limit:

| | allocation-free (heapsort / introsort) | stable (merge / timsort) |
|---|---|---|
| works under `@noheap` / `--no-heap`, MCU | ✅ | ✗ |
| multi-key sorting is correct | ✗ | ✅ |
| peers | Rust `sort_unstable`, Go `sort.Slice`, C++ `std::sort` | Rust `sort`, Go `SliceStable`, C++ `stable_sort` |

Every peer ships **both**. The spike should say whether kama does too, and if only one, which — bearing in
mind that "sorting a fixed buffer with no heap" is exactly the MCU/audio use case kama courts, and that a
silently-unstable sort produces wrong multi-key results without any error.

### Spike C — what `parseInt`/`parseFloat` return

*Survey what peers do AND argue what kama should do.* Rust: `Result<T, ParseIntError>` with
Empty/InvalidDigit/PosOverflow/NegOverflow. Go: `strconv.Atoi` → `(int, error)` with `ErrSyntax`/`ErrRange`.
C#: both `Parse` (throws) and `TryParse` (bool + out). Python: raises `ValueError`. JS: `parseInt` returns
`NaN` (widely considered a mistake). Zig: `std.fmt.parseInt` → `!i32` with `error.Overflow` /
`error.InvalidCharacter`.

**Note that the two languages closest to kama in philosophy — Rust and Zig — both distinguish OVERFLOW from
MALFORMED.** And kama's own doctrine (GOALS #3d) says `Optional<T>` is *absence* and `Result<T, E>` is
*failure*; a parse failure is a failure. The counter-argument is that `Optional` matches `string.find`, is
one obvious spelling, and most callers print a generic message anyway. Resolve it on the merits, not on
which is less typing.

### Bug fix — `substring` can produce invalid UTF-8

Not a design question; a defect with a decision attached. **Verified:**

```kama
string s = "A\u{E9}Z";                       // 4 bytes: 'A', 'é' (C3 A9), 'Z'
string cut = s.substring(start: 0, end: 2);   // len=2, second byte = 195 (0xC3)
```

That result is **not valid UTF-8** — a lone lead byte — produced from valid input, in the safe surface, with
no `unsafe` and no error. It is the ONLY such hole: `split`, `replace` and `find` all operate on whole
needles, so a valid needle in a valid haystack always lands on codepoint boundaries.

`substring` bounds-checks against `len` only
([kama_runtime.h:483](../../kama_runtime.h#L483)). Recommended fix, consistent with the language's existing
discipline (indexing **traps** rather than invoking UB; `Optional` is for absence, not for programmer
error) and with Rust, where `&s[0..2]` panics on a non-char-boundary: **trap on a non-boundary offset**.
The check is O(1) — a boundary byte must not be a UTF-8 continuation byte, `(b & 0xC0) != 0x80`. Ship with
a `tests/trap/` fixture. Confirm the breaking-change appetite first: code that currently slices
mid-codepoint would start trapping, though it is already producing invalid UTF-8 today.

### Still open, unchanged (leans only)

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

## Where kama differs from what an LLM will reach for

Each of these cost a build cycle while writing M1's fixtures — but read the classification before treating
them as defects. **Almost all are kama being deliberately STRICTER than C#/Rust/Python, and the strictness
is the point.** Exactly one is a genuine gap, and it is marked. This section exists to save the next
session those cycles, not to suggest the language is trappy.

**Genuine gap:**

- ⚠️ **A `match` SUBJECT must be a named local — a call result is rejected.** `match (pick(x: 1))`, where
  `pick` returns a plain enum, fails with "`match` requires an enum subject (a tagged union, or a plain
  enum)" — which is misleading, since it plainly *is* one. Bind first:
  `Code c = pick(x: 1); match (c) { … }`. Broader than SPEC § Known limitations describes (that lists only
  a nested value-producing `match` or a variant-producing ternary). Tracked in ROADMAP §2.

**Deliberate design — learn it, don't work around it:**

- **`public` field on a `type resource` is rejected** — a resource owns something, and a public field
  bypasses the invariant it exists to hold (GOALS 3c). Expose behavior through methods.
- **`@generate` requires EVERY field marked `@field` or `@skip`** — explicit over implicit, so adding a
  field can never silently start serializing a secret. (Rust's serde defaults the other way.)
- **Named arguments everywhere**, including where they read like noise: `println(s: "…")`,
  `FixedArray.make(size: …)`. This is the headline language feature, not friction.
- **String-interpolation holes are an identifier plus `.field`/`[i]` only** — `"${a.length()}"` is an
  error; bind the call's result first. Same boundary Rust's `format!` draws, and what keeps interpolation
  statically checked rather than a mini-language.
- **Generic free functions infer their type args** (`max(a: 3, b: 4)`); the turbofish is only for shapes
  like `decode::<T>` where inference has nothing to go on.

**Correct rejection, unhelpful message (worth fixing, cheap):**

- **`slot x;` then passing `ref x`** — rejected, correctly: `ref` borrows a value that is already live and
  a `slot` has none. `out` is the tool. The diagnostic is clear here; the trap is only that an LLM reaches
  for `ref` reflexively.
- **A free function cannot carry `when [T: Copyable]`** — bounds belong in the type-parameter list
  (`fn f<T: Copyable>(…)`); `when` gates conditional conformance on a generic TYPE's method, and a free
  function has no "sometimes". Correct, but the bare `syntax error, unexpected when` should say that.

**Not a language matter:**

- A `.d/` fixture is a DIRECTORY of files built together; a bare `namespace` in a single-file fixture will
  not resolve a sibling. Test-harness convention.

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
