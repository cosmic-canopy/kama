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
> anywhere (here, in a commit message, in someone's notes) keeps meaning what it meant. A new row takes the id
> on ROADMAP.md's **Next id** counter and bumps it in the same edit. This replaced position numbering, which silently re-pointed every
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

1. **A child's pipe without blocking** shipped in `0.9.786` (SPEC *Pipes and children on a fiber*). A served file's
   `stat`, `open` and `read` blocking a server's loop is KR-3 step 7's offload pool.
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
   constant-time half — ciphers, key exchange, signatures — which is `@kama/sodium`'s job. Recorded emphatically because TLS has been mistaken for scheduled work, listed with
   the status "ROADMAP" and waited on: `wss://` is not coming to `std`, and the answer for a
   secure socket is a package or terminating TLS at a reverse proxy. A non-goal that reads like a
   backlog item gets re-triaged forever. **Re-affirmed 2026-10-14**, with what each asked-for piece is instead: the
   browser's `fetch`/`EventSource`, an HTTP client and server, and a native WebSocket — the first-party `@kama/http`
   (KR-173); native WebTransport — a package on `@kama/tls`; a file chooser and native video/audio elements — the UI
   toolkit's packages, since std has no UI. No new language
   surface; pure library/codegen. The items below are what the campaign left open:
   - **`std::net`** — DNS **shipped `0.9.192`** as explicit `resolve`/`resolveOne` + `TcpStream.connectTo`
     (IPv4 only, because every socket seam is `AF_INET`; a name resolving to IPv6 alone reports
     `HostUnreachable` rather than an empty list). *(UDP and ephemeral-port `getsockname` ship —
     `lib/std/net/udp.kama`; IPv6 and multicast are separate, tracked in §2 — and DNS is the first
     concrete reason to want that row: the resolver already sees the AAAA records it has to drop.)*
   - **`std::net` local sockets and TCP options — the verdicts (shipped `0.9.482`–`0.9.485`).** The
     record is SPEC's net section; what stays here is what was decided NOT to ship, each with its reason:
     `UnixStream.pair()` (socketpair) was genuinely optional until `std::process` could hand a descriptor to a child —
     between isolates a `Channel` answers, and a pair's use across processes is exactly that hand-off; it can since
     `0.9.624` (`Stdio::Handle`), so the pair shipped in `0.9.653` (`UnixStreamPair`, SPEC's net section); (a
     non-blocking Unix `connect` was filed here as optional, and that was WRONG — a blocking connect to a full
     queue waits on Linux, where the caller never sees the `EAGAIN` it was told to retry; it has shipped);
     getters for the keepalive and
     user-timeout knobs are genuinely optional — no std option has one, and a program sets what it wants;
     (`UnixDatagram` implementing `DatagramSocket` shipped in `0.9.650`, by making the contract generic over its
     address type.) And one ruling that outlives
     the rows (the maintainer, 2026-09-30): **a platform is never cut down to the common subset** — the primary
     interface is the same everywhere and each OS's deltas live in the per-platform TYPES it returns
     (`PeerCredentials`, `UserId`, Linux's `UnixAddr` with abstract names, POSIX's `UnixDatagram`), because
     `@compileFor` gates types, not members, by design.

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

<a id="s1-compression"></a>

### Compression and archives past deflate, zlib, gzip and tar: zstd (KR-212), brotli (KR-213) and the rest

| | deflate · zlib · gzip | tar | zstd | brotli |
|---|---|---|---|---|
| Go | std, both directions | std | internal decoder; a proposal | — |
| Python | std, both | std | std since 3.14 (`compression.zstd`, PEP 784) | — |
| Zig | std (compression removed in 0.15, back in 0.16) | std | std, decode | — |
| C# | std | std (`System.Formats.Tar`) | — | std |
| Java | std | — | — | — |
| Node | std | — | experimental | std |
| Rust | crates | crates | crates | crates |

What shipped, deflate, zlib, gzip, tar and `extractInto`, is in [SPEC.md](SPEC.md) and its reasoning in the git log. What is below builds on its shape:
a decoder over a `BufReader<R>` with a required `limit`, a `finish` that reads nothing, an encoder's level as a typed `@nonExhaustive` enum.

#### Left out of deflate, zlib, gzip and tar, each with its verdict

- **Preset dictionaries** (RFC 1950 FDICT, raw-deflate dictionaries): **scheduled, KR-257.** zlib, Go, Python, Java and Node take one, and permessage-deflate's context
  takeover is one by another name.
- **zip**: **scheduled, KR-258.** jar, docx, epub and Python wheels are zip; Go, Python, Java, C# and Zig ship it. It sits on std's deflate and crc32.
- **xz and bzip2, decode**: **scheduled, KR-259,** after KR-213. kernel.org and the distributions ship `.tar.xz`, and a program must read what others send. Python ships both,
  Go bzip2 decode, Zig xz and lzma decode.
- **Sparse files, and other pax records** (`SCHILY.xattr.*`, which OCI layers carry): **scheduled, KR-303.** A sparse file reads as `TarKind::Other` with its raw form as the
  body, where Go, Python and Rust's `tar` expand it, and VM images use them.
- **GNU's incremental, volume and multi-volume types**: **genuinely optional,** since `Other` lists them and a body is still readable, and no format a program receives today needs more.
- **Writing a gzip name or an mtime**: **genuinely optional.** RFC 1952 makes them advisory, and a container, tar, is where names and times belong.
- **`extractInto` taking the last of a duplicated name**, as GNU tar, bsdtar and Python do: **a non-goal,** ruled with `CreateNew`. A listing that shows one file and an extraction that
  leaves another is how a scanner is fooled, and a program that wants those archives lists them with `TarReader` and decides.

#### KR-212 and KR-213, ruled provisionally (re-probed when each is built)

- **Z1.** `std::compress::zstd`, the same shape: a `BufReader<R>` source, a required limit, `InvalidData`/`LimitExceeded`.
  Concatenated frames are one stream, and skippable frames are skipped.
- **Z2.** A frame declares its window (RFC 8878 allows up to 2^41 bytes). The decoder allocates at most min(declared
  window, `limit`), and refuses a window over 128 MiB, the reference decoder's default (`ZSTD_WINDOWLOG_LIMIT_DEFAULT`
  27). That needs no second parameter, because the window can never usefully exceed the output. HTTP's zstd coding caps
  the window at 8 MiB (RFC 9659), which a caller's limit already enforces.
- **Z3.** The XXH64 content checksum is verified when present, and `std::digest::xxhash64` becomes public beside crc32.
  A frame that names a dictionary is `InvalidData` until a dictionary row. The encoder keeps its own verdict.
- **B1.** `std::compress::brotli`, the same shape. The window is at most the RFC's 16 MiB, and the large-window
  extension is refused.
- **B2.** The 122,784-byte static dictionary becomes a `comptime InlineArray<uint8>` literal. Measured 2026-10-29 with a
  stand-in of that size: the release build went from 0.10 s to 0.41 s, the binary grew 115 KB, and the literal is
  580 KB of source. Building it will split that cost between the kama front end and clang. If kama dominates, an
  embed feature (Zig `@embedFile`, Rust `include_bytes!`, Go `//go:embed`, C23 `#embed`) gets its own row and verdict:
  shaders and web assets want the same thing.
- **B4 (2026-10-31).** CVE-2026-111497 ("Node2Shell", CVSS 4.0 9.2): a crafted stream made Google's C decoder
  (`c/dec/decode.c`, 1.0.3 through 1.2.0; fixed in 1.0.10, 1.1.1, 1.2.1) write outside its state, reached through
  Node.js, Bun, .NET, Python's `Brotli` and `brotlicffi`, and Envoy. kama's decoder is kama: every buffer access is
  bounds-checked, so the same mistake traps instead of writing. The CVE's published proof-of-concept stream and the
  reference test vectors become `std::compress::brotli` fixtures that must answer `InvalidData` (or decode), under the
  sanitizer leg, and the decoder is a first target for coverage-guided fuzzing (KR-205) when that lands.
- **B3.** kama's decoders run on every target now. In a browser, an HTTP body's `br` or `zstd` is decoded before wasm
  sees it, and `DecompressionStream` is asynchronous where a kama `Reader` is not. The browser's own decoders come later,
  under the platform rule.

#### A faster CRC-32 (KR-302)

`Crc32` takes one table lookup per byte: about 570 MB/s on the M4 (14 ms for 8 MB, measured in step 4 above), which is over a third
of decoding a gzip file, though under 4% of encoding one. zlib folds with carry-less multiplication where the CPU has it and uses a
braided table where it does not; Go slices eight bytes at a time and uses hardware instructions where it can. Slicing-by-8 is the
step that fits std: pure kama, and its eight tables baked at compile time, as the one table is. Measure it interleaved, per byte
and through `GzipReader`, against the one-table loop, since every access in it is bounds-checked. `std::digest::crc32` is
public, so a faster one reaches PNG, zip and anyone else's checksum as well.

<a id="s1-http-package"></a>

### `@kama/http` — a first-party HTTP package for both targets (KR-173)

Decided 2026-10-14. **HTTP stays out of std on every target**, as the non-goal above says: std carries the
TRANSPORTS (TCP, UDP, DNS and the `Poller` natively; WebSocket, WebTransport and the DataChannel in the browser) and
the building blocks (SHA digests, base64, JSON, the `std::io` streams). The browser's own HTTP APIs, `fetch` and
`EventSource`, are HTTP clients, so they belong with HTTP, not with the browser transports: a std `fetch` would be an
HTTP client on one target only. Peers split down the middle (Go, Zig, C# and Python ship HTTP; Rust, C and C++ do
not, and Rust's browser `fetch` is the `web-sys` crate). **The web-first answer is a first-party package**, as
`@kama/tls` is: `fetch`/`EventSource` in the browser (its JS through `jsLibraries`), HTTP/1.1 client and server
natively on std's `TcpStream`/`Poller`, server-sent events, a native WebSocket client and server (the RFC 6455
upgrade on std's `sha1` + base64), and TLS through `@kama/tls`. Native WebTransport (QUIC + TLS 1.3) is a package on
`@kama/tls` too, not std.

**Streamed bodies (2026-10-15).** The server reads a request body in pieces as it arrives and writes a response
in pieces, with the body limit set per route. A server that reads a whole body before the handler sees it caps
every upload at one limit and holds it all in memory. A program can chunk uploads at the application level to get
around that, but the package should not require it.

### Reported by an HTTP package, 2026-10-18 (KR-202..208)

Each was run on `0.9.668` on macOS and, where the OS matters, on Linux (the `localhost/kama-dev` container), or shown
absent from std's source.

<a id="s1-async-dns"></a>

#### A DNS lookup a loop can poll, with a deadline (KR-202)

`resolve` is `getaddrinfo`, and its own comment says it "BLOCKS, possibly for seconds, and cannot be given a
timeout … Resolve on another isolate". A client inside a server's loop needs a lookup it polls like any socket.
libuv and tokio run `getaddrinfo` on a worker thread; Go has its own resolver; Rust's std blocks. Recommended: the
worker form, completing through the `Poller`'s waker (`0.9.719`, SPEC *Readiness*), with a deadline after which the caller stops waiting (the worker
finishes and its answer is dropped). No DNS exists in a browser, so the request is native-only.

<a id="s1-url"></a>

#### Percent-encoding and a URL type (KR-204)

std has no URL or URI type and no percent-encoding (`lib/` and `include/` searched; only base64's URL-safe alphabet
mentions a URL). A URL is not HTTP's alone: std's `WsConnection.connect(url)` and `WtConnection.connect(url)` take one
as a bare string, and connection strings and `file:` URLs are URLs too.

**What other languages do.**

| | where | grammar |
|---|---|---|
| Go | std `net/url` (`PathEscape`, `QueryEscape`, `Values` for forms) | RFC 3986, "except where it deviates for compatibility" |
| Python | std `urllib.parse` | RFC 3986, lenient |
| Zig | std `std.Uri`, which `std.http.Client` takes | RFC 3986 |
| C# / Java | `System.Uri` / `java.net.URI` | RFC 3986 (C#), RFC 2396 (Java) |
| Swift | Foundation `URL`, `URLComponents` | RFC 3986 since iOS 17 / macOS 14, with a strict mode (`encodingInvalidCharacters: false`) |
| curl | its own URL API since 2018 | RFC 3986 since 2005 |
| C++ | none in std; Boost.URL (strict RFC 3986), ada (WHATWG, Node's parser) | — |
| Rust | none in std; the `url` crate (WHATWG), `http::Uri` (request-target) | — |
| JS, browsers | the global `URL` | WHATWG URL Standard |

The two grammars disagree on real input. Given `http://EXAMPLE.com/a b\c`, RFC 3986 has no component that allows
the space (0x20) or the backslash (0x5C), so the input is not a URL; WHATWG repairs it to `http://example.com/a%20b/c`
(host lowercased, the space encoded, `\` read as `/` for a special scheme). Two parsers that disagree are an attack
class, not a style difference: Orange Tsai's "A New Era of SSRF" (Black Hat USA 2017) found zero-days in the URL
parsers of Python, PHP, Perl, Ruby, Java, JavaScript, wget and curl by feeding one URL to a checker and a fetcher that
read it differently. curl's answer was one parser for every decision in a program.

**Verdict (the maintainer, 2026-10-18): std, strict RFC 3986, plus form encoding.**
1. **std.** A URL is the address type of std's own transports, and most standard libraries carry one.
2. **Strict RFC 3986.** `Url.parse` refuses what RFC 3986 does not allow, naming the byte and its offset, and never
   repairs: WHATWG's repairs are exactly where parsers disagree, and a silent rewrite is implicit. RFC 3986 is also
   the grammar HTTP itself uses (RFC 9110's `http`/`https` URIs). A component is kept as written; decoding and
   comparison (case, percent-encoding normalization) are explicit operations.
3. **Percent-encoding in `std::encoding`**, beside base64 and hex, with RFC 3986's per-component sets (path segment,
   query, fragment, userinfo). Encoding text a user typed is an explicit call, not a parser's leniency.
4. **`application/x-www-form-urlencoded` too** (space as `+`): a separate format, defined by the WHATWG URL Standard's
   §5, which every server reads from form bodies and queries. Go and Python ship it beside their RFC 3986 parser.
5. **`WsConnection.connect` and `WtConnection.connect` take a `Url`**, not a string.
6. **IDNA (non-ASCII hosts) is a later, explicit conversion**, since RFC 3986 is ASCII.

Size M. What remains to decide at build time is the surface (`Url`'s accessors, reference resolution per RFC 3986 §5
for redirects, normalization per §6), which follows from the above.

<a id="s1-freebsd-balance"></a>

#### FreeBSD's `balance` (KR-227)

Listener options were ruled 2026-10-21 with `balance` on Linux and FreeBSD, and shipped in `0.9.713` (SPEC *Listener
options*) for Linux alone. FreeBSD's
balanced port sharing is `SO_REUSEPORT_LB` (FreeBSD 12+; its manual, unmeasured), not `SO_REUSEPORT`, which there
behaves as macOS's does. kama detects a FreeBSD host (`src/kama.driver.cpp`) but has no built-in FreeBSD target and no
FreeBSD leg, so a `@compileFor(OS_FREEBSD)` twin would compile nowhere this repo runs. It falls into the
`!OS_LINUX, !OS_MACOS` twin today: the backlog alone, which is correct, only narrower than FreeBSD can do. To build:
a FreeBSD leg (a CI runner or the container), then the twin, its `kama_set_reuseport_lb` helper, and the fixture's
`shared()` branch. The same leg owes `cpuCount()` its FreeBSD half: `cpuset_getaffinity`, as Linux and Windows read
their masks (`kama_isolate.h`); FreeBSD falls into the `sysconf` branch today, which sees no mask.

<a id="s1-fuzz"></a>

#### Coverage-guided fuzzing (KR-205)

`kama --help` has no fuzz command, and no doc mentions libFuzzer. A parser that reads bytes from strangers wants a
coverage-guided target. Without one, a package runs a seeded mutation loop under an ASan build, which finds only what
random bytes find. **Design first.** The questions:
- the entry: a `fn` over a `ConstView<uint8>`, with a marker or a manifest key;
- whether `kama fuzz` drives it or a documented harness does;
- toolchain reach: clang's `-fsanitize=fuzzer` needs a clang with the libFuzzer runtime. Linux's and Homebrew's
  LLVM have one, and Apple's clang does not;
- how a corpus and crashes are kept in a project.

<a id="s1-std-dirs"></a>

#### The standard directories: temp, config, cache (KR-266)

Filed 2026-10-30, from two programs that needed them. std names no temporary directory and no per-user configuration
or cache directory, so a program reads `TMPDIR` or `APPDATA` itself, which a parent process can change. Go's `os` has
all of them (`TempDir`, `UserConfigDir`, `UserCacheDir`, `UserHomeDir`); Rust's std has `env::temp_dir()` and leaves
the rest to the `dirs` crate; Python has `tempfile.gettempdir()`. **Recommended shape:** `tempDir()` (`TMPDIR`, else
`/tmp`; `GetTempPath2W`), and `userConfigDir()` / `userCacheDir()` (XDG with its fallbacks; `~/Library/Application
Support` and `~/Library/Caches`; the Known Folder API's RoamingAppData and LocalAppData), each a `Result<string,
IoError>`; and a `TempDir` resource that makes a fresh private directory (`mkdtemp`) and removes its tree on drop.

<a id="s1-time-zones"></a>

#### Time zones (KR-267)

Filed 2026-10-30. `std::time` has instants, durations and the civil calendar, but no zone: no lookup by IANA
name, no local zone, no offset at an instant. A database client needs it for a zoned timestamp, a server for a user's
local time. Go's `time.LoadLocation` reads the system's TZif files, with an embeddable `time/tzdata`; Java ships
`java.time.ZoneId` with bundled data; Rust leaves it to `chrono-tz` and `jiff`. **Design first:** a `TimeZone`
looked up by name (TZif v2+ with its POSIX footer rule from `/usr/share/zoneinfo`; on Windows the ICU it ships since
10 1903, or bundled data), the local zone (`TZ`, `/etc/localtime`, `GetDynamicTimeZoneInformation`), and the offset
and abbreviation at an instant, with a civil time in a gap or a fold answered explicitly rather than guessed.

<a id="s1-secure-zero"></a>

#### Wiping secrets (KR-271)

Filed 2026-10-30, from a TLS package's own triage. Nothing in std zeroes memory so that the compiler cannot remove the
store, and a growing byte buffer leaves the old bytes in the block it freed. C11 has `memset_s`, glibc and the BSDs
`explicit_bzero`, Windows `SecureZeroMemory`, libsodium `sodium_memzero`; Rust's answer is the `zeroize` crate.
**Recommended shape:** a `wipe` over a `View<uint8>` that the optimizer cannot elide, and a byte buffer that wipes on
drop and wipes its old block before freeing it on every reallocation. `constantTimeEquals`, already in std, is its sibling.

<a id="s1-cpu-time"></a>

#### A process's CPU time (KR-272)

Filed 2026-10-30. std cannot say how much CPU a process has used, user and system time, which a benchmark and a
resource limit both need; std has a process's liveness and start time, not this. C has `getrusage` and `clock()`, Windows
`GetProcessTimes`, Python `time.process_time()` and `resource.getrusage`; Rust and Go leave it to crates or
`syscall.Getrusage`. **Recommended:** the current process's user and system time as `Duration`s
(`getrusage(RUSAGE_SELF)`, `GetProcessTimes`), and a waited child's from its exit (`wait4`'s rusage).

<a id="s1-fs-root"></a>

#### Opening a file under a root without a race (KR-311)

Filed 2026-11-04. A server of static files takes a path from a stranger and opens it under a root. `File.open` takes a path
and follows links, and `canonicalPath` and `symlinkStat` answer about a path, so the check and the open are two walks, and a
link swapped between them leaves the root. Measured on `0.9.842` with the swap made between the two calls: the check says the
page is under the root, and a read of that same name returns the file outside it. `File.metadata().id` can show afterwards
which file opened, which narrows the race and does not close it. No `openat` or `O_NOFOLLOW` reaches a program; the runtime's
one `O_NOFOLLOW` sets a new directory's mode.

Two of std's own promises stop at the same place. `removeDirAll` says so in its comment ("needs `openat`/`unlinkat` against a
directory HANDLE"). `extractInto` asks `symlinkStat` of each parent and then opens by path, which settles what an archive can
do and leaves the same window for a second writer in that directory while it runs (read from the source, not run).

| | Under a root (from their documentation, not run here) |
|---|---|
| Go | `os.Root` (1.24): a name that leaves the directory is an error; a link is followed, and may not leave |
| nginx | `disable_symlinks on from=$document_root`: any link in the path is refused; needs `openat` and `fstatat` |
| Linux | `openat2` with `RESOLVE_BENEATH`, since 5.6 |
| Rust | nothing in std; the `cap-std` crate's `Dir` |

**The verdict: scheduled, design first.** The questions: whether a link that stays inside is followed (Go) or every link is
refused (nginx's `on`, the smaller form); the per-platform types, since Linux has `openat2`, other POSIX systems walk each
component with `openat` and `O_NOFOLLOW`, Windows opens relative to a handle, and wasm's virtual filesystem has neither; and
whether `removeDirAll` and `extractInto` move onto it, which would close their windows too.

<a id="s1-string-from-bytes"></a>

#### A `string` from owned bytes without a copy (KR-273)

Filed 2026-10-30. Every way from bytes to a `string` copies them (`utf8::decode` validates and copies,
`kama_string_from_raw` copies), so a program that already holds UTF-8 in a `DynamicArray<uint8>` pays a copy, and one
that knows its bytes are valid pays the validation too. Rust has `String::from_utf8(Vec<u8>)`, which validates and
moves the buffer, and `unsafe from_utf8_unchecked`; Go's `string(b)` copies and `unsafe.String` does not. std has the
mechanism inside it since `0.9.779` (a `StringWriter`'s consuming `finish` adopts its buffer); this row is the public
surface: a moving constructor that validates and hands the bytes back in its error, and an `unsafe` one that does not.

<a id="s2"></a>

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The
language-completeness residual is **closed**; what remains here is genuinely later-track or opt-in.

### Verdicts on reported gaps (2026-10-08)

- **A scoped `unsafe { }` block is a NON-GOAL.** It existed and was removed: one block disabled definite
  assignment for its whole function, and `unsafe` became a property of the function so that the inventory is
  greppable at declarations (GOALS §3a). The need is a safe type whose methods all reach one raw field. The answer:
  put the C-touching code in private `unsafe fn` helpers and make the public methods safe callers of them. Calling
  an `unsafe fn` from safe code is unrestricted.
- **Per-function optimization (`@optimize`) is a NON-GOAL.** C has no portable spelling: `optimize` is GCC-only and
  clang offers only `optnone`. `debug.optimize` (docs/targets.md, `0.9.564`) gives the package granularity instead.
- **A workspace member's path dependency on a directory the workspace does not list is refused, BY DESIGN.** A
  member is extractable only when what it path-depends on is declared beside it (docs/packages.md), and the remedy
  is one line in `kama_workspace.json`. A consumer does not restate a dependency's own path dependencies. Measured
  on `0.9.542`: a member depending on `audio`, which path-depends on `voice` (C sources included), builds without
  naming `voice`. A consumer that imports `voice` itself must declare it.

### Verdicts on reported gaps (2026-10-14), each measured on `0.9.577`

- **A gated-out `csources`/`cincludes` path must still exist, BY DESIGN** (docs/targets.md): a path that is wrong only
  on the platform nobody builds daily is exactly what a gate would otherwise hide.
- **A build with no `--target` applies `select.TARGET.HOST`, never the `LINUX` or `MACOS` arm, BY DESIGN.** A target
  name is how a build was spelled; a per-OS setting gates on the fact (`OS_LINUX`). `csources` and `cincludes` take a
  fact gate, and since `0.9.609` so do `link` and a dependency edge (docs/targets.md), which is where this was felt.
- **A path dependency of a path dependency across separate repositories is restated at the top, BY DESIGN**
  (docs/packages.md): a fetched package cannot carry a local path. A `kama_workspace.json` holding both packages
  permits it, and registry versions end it.
- **`scope` is in the shipped agent guide's reserved words**, since `0.9.557`; the report read a guide that lists it.

<a id="s2-contract-narrow"></a>

### A boxed contract cannot be narrowed back to its type (KR-314)

Reported 2026-11-04 by a game engine on kama; reproduced on `0.9.844`. Erasure works: a `DynamicArray<Owned<Column>>` holds
`new Store::<Pos>.make()`, and `cols[0].length()` dispatches. The way back does not: `Optional<Store<Pos>> back =
cols[0].as<Store<Pos>>();` is refused, "`.as<T>()` applies to a boxed error". On an `Error` it answers a copy and refuses a type that owns
memory (SPEC, `xfail/as_downcast_uncopyable`), so a store could not come back even there. Storage whose types are chosen at run time (an
archetype store, an inspector, mods, a project loading a type the program never named) needs a typed way back, and today it takes
`UnsafePtr` and hand-rolled type ids, outside kama's checks. Rust has `Any::downcast_ref`, C# `as`, Go a type assertion.

**The verdict: scheduled, design first.** Asked for: any boxed contract, answering a borrow rather than a copy. To settle: where the type
identity lives, whether it costs a contract that never narrows anything, and how the borrow roots under the window rules.

<a id="s2-immutable-array"></a>

### An `immutable` type cannot hold an array (KR-329)

Reported 2026-11-05 by a game engine on kama; reproduced on `0.9.852`. `Shared<immutable T>` is how many isolates read one large read-only value with no
copy: SPEC's table of how state reaches an isolate gives it to every isolate, unbounded, because nothing can change. But `type immutable value Clip { public InlineArray<float32>#(4) keys;
public int32 n; … }` is refused, *"`immutable` type `Clip` has a mutable member `keys` of type `InlineArray<float32>#(4)` — every part of an `immutable`
type must itself be deeply immutable"*, and a `DynamicArray` in a `type immutable resource` the same way. A `string` field builds, so untyped bytes are
the only array an immutable type may carry: no typed table, mesh, animation clip or navmesh. It matters more under KR-3: a `scope`'s children will share
a `const ref` (decision 10), but a held `Job<R>` takes a moved `Sendable` bundle and cannot borrow, so work that spans frames reads shared data only
through `Shared<immutable T>`. Unity's blob assets are the precedent (*"immutable binary data… you can access them safely in parallel"*, with `BlobArray`),
and Rust's `Arc<[T]>` is the same idea without a type rule.

**The verdict: scheduled, design first** (the maintainer: "a general good thing for kama to have as a general systems language"; any program sharing a
large read-only table across threads needs it). The least is an `InlineArray` whose elements are immutable, which kama already admits among the fields
every isolate may share in a global allocator (SPEC, *Global allocator*). The rest is an array frozen once it is built, for sizes known only at load. To
settle: how a frozen array is spelled and built, and whether freezing is a type or a state.

<a id="s2-infer-from-bound-arg"></a>

### An argument typed by a parameter another argument fixed (KR-285)

Found 2026-11-01 beside a generic reading a callee's type parameter as its own (fixed in `0.9.804`), and not the same
thing: it fails with the parameter renamed, and in plain code:

```kama
DynamicArray<int64> o = DynamicArray.empty();
DynamicArray<int64> t = replace(place: ref o, with: DynamicArray.empty());   // "cannot infer `T` — argument `with` is …"
```

`place` fixes `replace`'s `T` as `DynamicArray<int64>`, but the `with:` argument is not typed from that binding, so the
bare `DynamicArray.empty()` has no type arguments to build with. A literal is typed by its destination elsewhere; here
the destination is a type parameter another argument has already bound. Rust infers `mem::replace(&mut v,
Vec::new())`. A missing capability with a spelled workaround (`DynamicArray::<int64>.empty()`), so a feature row, not a
defect; unscoped — the order in which `inferGenericInst` binds and then types its arguments is the place to look.

<a id="s2-probe-inner-calls"></a>

### The as-written probe solves a generic call inside a generic body (KR-286)

`checkTemplatesAsWritten` walks a generic nobody instantiates (a library's) with each type parameter bound to an opaque
type, so the body is checked without a type argument. A generic call inside it is solved there only when written with a
turbofish (`ident::<Optional<T>>(x: a)`, solved on demand by `explicitGenericInst`). An inferred one (`ident(x: a)`) is
never solved, so its arguments, its arity and the use of its result are checked only per instantiation. Since `0.9.804`
its result reads as an open type, a name nothing declares, and the probe defers on it. Before that, the callee's `T` was
read as the caller's (fixed in `0.9.804`). Rust and Swift check a generic body this way: the outer parameters are rigid
type variables, and `ident(x: a)` with `a: Optional<T>` types as `Optional<T>`.

**Measured 2026-11-01.** In `genericCallReturnNode`, solving the call on demand while probing (`inferGenericInst` over
`_localTypeNodes`, `explicitGenericInst` for a turbofish) raised two false errors at once, on
`tests/generic_param_name_capture.kama`. `Holder<T>.get()`'s `ident(x: this.o)` got "cannot infer generic type
parameter 'T' — argument 'x' is not a literal or a locally-typed value". The probe's emission walk types no `this.field`
(`_scanThisCls` is a discovery-walk table), and inference reports when it fails. Over the 1,165 top-level fixtures, the
`0.9.804` fix moved one probe site from resolved to deferred (that fixture's `viaMethod`) and no other template's tally.
So nothing in the corpus was being checked only through the capture.

**What it needs, in order:** an inference that stays silent when it cannot solve (the call then stays open, as now);
`this` and its fields typed in the probe walk; the solution recorded in `_callInst`, so emission routes the call and
checks its arguments as the turbofish arm does; then the gate: zero new diagnostics over the corpus on bodies that
compile, with `--probe-templates` counting the sites it moves from deferred to resolved.

<a id="s2-else-if-ladder"></a>

### An `else if` ladder stops at about 195 arms (KR-307)

`if (a == 0) { … } else if (a == 1) { … } …` is right-recursive in the grammar, so each arm holds six symbols on the parser's stack, which `YYINITDEPTH` fixes at 1,200. Measured on `0.9.838`, the same on macOS and
Linux: 195 `else if` arms build and run, and one more is refused with a message saying so. Nesting does not cost that much elsewhere: 294 nested calls, 1,180 nested blocks. A decision table from a generator reaches 195.

Measured with the same ladder at 198, 1,000, 5,000 and 20,000 arms. Apple's clang, node and python took 1,000 and failed at 5,000 (a stack warning and error, a `RangeError`, a `RecursionError`), zig's `ast-check` took 5,000 and died with SIGSEGV at 20,000, and rustc passed analysis at 20,000. So kama is alone below 1,000.

Why it is low: `YYINITDEPTH` cannot simply rise, because `YYSTYPE` holds `shared_ptr`s, so Bison's stack cannot relocate and all 1,200 entries are built on every parse. That costs 0.5 ms a parse at 1,200 (see the note at the top of `kama.y`), and `kama lsp` parses per keystroke.

The likely fix is a left-recursive ladder rule, so that it leaves the stack flat, with each arm a chain link as `CHAIN_LINK` counts one. The node each arm builds stays what it is. What the C compiler takes then decides the number, since the ladder's text is flat but clang recurses through it: Apple's took 1,000 and warned of its stack by 5,000. A grammar change is four artifacts if the grammar's text changes.

<a id="s2-contextual-spawn"></a>

### `spawn` names a method (KR-337)

`type value Pool { public fn void spawn(int32 n) { … } }` is refused at the declaration ("`spawn` is a reserved word,
so it cannot be used as a name here"), and so is `p.spawn(n: 2)` (measured `0.9.860`); the same file with `truncate`
builds, because `truncate` is contextual (`docs/KEYWORDS.md`). Engines spell creating an entity `spawn` (Bevy's
`commands.spawn`, Unreal's `SpawnActor`), so it is the first name a game author or an agent reaches for. Reported by a
game engine on kama, which spells it otherwise meanwhile, so nothing is blocked.

**The rule,** worked out on `0.9.860` and not yet built: `spawn` is the keyword where a spawn can start, before a name
(`spawn f(…)`, `spawn fiber …`, a future `spawn job`) or before its one clause (`spawn(stack: …) f(…)`), and a name
everywhere else, always so directly after a `.` (`p.spawn(stack: 3)` is a call). The lexer decides it in
`contextualWord` beside `truncate`, from the text after the word and from where the last `.` ended; a `SPAWN_NAME`
token joins `name_word`, which admits it wherever `truncate` is admitted as a name. The one spelling it cannot take
is a free function named `spawn` called with `stack:` as its first label, which reads as the clause.

**Why it is a row and not done:** the editor grammars are most of it. tree-sitter decides `give`/`copy`/`truncate` in
its external scanner (`tree-sitter-kama/src/scanner.c`), so `spawn` joins that scanner, `parser.c` is regenerated with
the pinned CLI (`npm --prefix tree-sitter-kama ci`), and `editor/zed/extension.toml`'s pin follows the grammar
commit (`tools/check-editors.sh`), as the else-if ladder's note says a grammar change is four artifacts. The machine
that measured it has no tree-sitter CLI; the one that has it takes the row.

<a id="s2-quoted-format"></a>

### A quoted, structural rendering — `${x:?}` (KR-222)

`Formattable` is kama's one to-text contract, and its derive renders a string raw: `Stat { name: a, b }` cannot be told
from two fields, and since collections render (`0.9.700`), `[a, b]` prints for both `["a", "b"]` and `["a, b"]`. Logs want a form that cannot
be misread. Rust has `{:?}` (`Debug`), Python `repr`, Swift `debugDescription`, Go `%q`/`%#v`; Java and Kotlin have
none. SPEC has called `${x:?}` "a possible additive future" since the derive shipped; today `${s:?}` is a lexical error.

Scheduled 2026-10-20 (the maintainer, ruling the collections' quoting a follow-up). Design first: whether it is a second
contract or a mode of the one sink (`Formatter` already carries the specifier), what a user type writes to take part,
how a string escapes (JSON's rules are the obvious candidate), and that structs, enums and every collection take it
together — never one at a time.

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
    because it let ANY module reach into a type it does not own. (`type adapter`, `0.9.511`, brings that
    reach back for one module only — the one that declares the contract — and only as a conformance: named
    methods, reached solely through that contract. A specialization would still be a silent body swap.) A specialization lets a module reach into
    a *function* it does not own and change what that function does for a given type — program-wide,
    invisible at every call site, from any package that can see the generic. Retro-impl at least added a
    *named method* you could see on the type; a specialization silently replaces a body. Whole-program
    coherence catches a *duplicate* specialization and does nothing about a single one, which is the
    dangerous case.
  - **What is left over is contract design, which is the language's answer already.** `type contract`
    plus `type adapter` covers primitives, `string`, every type you own, and — from a contract's own module
    — every type you don't. That is exactly what
    `std::math`'s `Real` is, and [scalar.kama](../lib/std/math/scalar.kama) says so in prose. The
    genuine remainder is a second BODY for a generic function you do **not** own, chosen by its type
    arguments, and unlocking that is the thing we do not want.
  - **Nothing depends on it.** Comptime parameters' three blockers are all comptime-parameters-on-types issues;
    the view-escape check is independent; no site in `lib/`, `prelude/`, `tests/`, `examples/` or
    `bench/` needs it.

  Reopen only if a concrete case appears that a contract genuinely cannot express. Three real defects
  came out of scoping it and have shipped: a duplicate function declaration was silent (for a generic
  template the last body simply won), a function type parameter's `= Default` was parsed and dropped,
  and a type parameter shadowing a visible type said nothing. All three are pinned by `tests/xfail/`.

- **A `type adapter` outside its contract's module — deliberately NOT built (user ruling, 2026-10-01).** An
  adapter lives only in the module that declares its contract (SPEC § *`type adapter`*). kama would not need
  the rule for SOUNDNESS — the whole-program view sees two claims of one (type, contract) pair and refuses
  them — but seeing a conflict is not being able to fix it: two libraries that each adapt `Timestamp` to a
  driver's contract leave an application that owns neither unable to build, and a library that adds an
  adapter in a new release would break every consumer that had written its own. Swift is the measured
  precedent: it allowed retroactive conformance anywhere and, in Swift 5.10 (SE-0364), began warning on a
  conformance of a type you don't own to a protocol you don't own unless marked `@retroactive`. Starting
  strict is the reversible choice — loosening breaks no one, tightening breaks everyone. **The relaxation
  path, if the rule ever chafes:** let the build's ROOT package (an application, which nothing depends on)
  adapt a foreign type to a foreign contract, while a library still may not. No diamond can form through an
  app, and its owner can delete their adapter the day a library ships one. Until a real program needs that,
  the answer is the one Rust gives: wrap the type in one of your own.

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
  - **`Optional` has no unwrap.** `match` is the only way to open it — there is no `?` operator. *(This said a
    generic enum "rejects members"; that stopped being true once enums took members, and the maintainer has ruled that `Optional`
    and `Result` gained yes/no queries only — `isSome()`, `isOk()`, `0.9.613` — while taking one apart stays `match`.)* `unwrapPtr` is monomorphic
    for the old reason, and the compiler emits calls to it directly into C at **nine** sites, so its
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

- **Unicode module (post-1.0).** The shipped `string` core is UTF-8 bytes + `.chars()` codepoints with
  **ASCII** casing/whitespace. `std::unicode` exists since `0.9.506` with NORMALIZATION (SCRAM's
  SASLprep needs NFKC), generated from one pinned UCD and held to its NormalizationTest.txt; what remains for it is
  Unicode-correct casing + whitespace, and an eager `DynamicArray<string>` collect for `split` (the lazy `Split`
  iterator ships today). Its tables reach only a program that imports it — a module's C is included by the TUs
  that call it — so a table in `std` costs an MCU build that does not import it nothing.
  **Grapheme-cluster segmentation belongs here too.** `substring` traps on a split *codepoint* and
  `floorCharBoundary`/`truncate` snap to one (SPEC § *Strings*), which guarantees valid UTF-8 but **not**
  visually intact text — a boundary cut can still split an `e` + combining accent, an emoji ZWJ sequence
  or a flag. Cluster boundaries are defined by UAX #29 and need the `Grapheme_Cluster_Break` property per
  codepoint, i.e. a data table — not a bit trick. A cheap partial version (range-checking the combining
  diacriticals) would be wrong for emoji, flags, Hangul and Indic while *looking* like a guarantee, so it
  is deliberately not shipped. Same stance as Zig, and utf8everywhere points at ICU for it. When it ships, it is
  `std::unicode`'s, generated from the same pinned UCD and tested against its GraphemeBreakTest.txt.
- **Stdlib layering — 3 LOW-prio follow-ups.** The prelude-vs-`std::`-vs-primitive split is principled and
  documented in [FLOOR.md](FLOOR.md) § "What is floor, and what is an `import`"; nothing is mis-placed. What
  is left, none of it blocking:
  - **`Atomic` needs no pthread but rides in `std::concurrent`**, which does — so lock-free-without-threads
    is unreachable. Split it to a `std::concurrent::atomic` leaf, or promote it as an always-available seam.
    Only matters once the multicore-MCU track starts (§5).
  - **No GPU module in std, by decision** (2026-10-16): a safe WebGPU binding and a window are first-party
    packages, `@kama/gpu` and `@kama/window` (KR-25, KR-26), and the C seam left std in `0.9.652` to become the
    WebGPU example's own platform layer (§8).

  Decided NOT to add a convenience-import of the common containers — explicit per-symbol imports stay.
- **Format/interpolation follow-ups (on the shipped `std::fmt` substrate).** Interpolation, format specifiers,
  `@generate(Formattable)`, and tagged strings all ship (SPEC), on every subject including a generic and an
  `enum` (0.9.246 — see *Derive follow-ons* above). Settled by the audit (2026-09-07). Shipped 0.9.226: combining a
  base marker with width/flags (`${n:08x}`). Not offered, by SPEC: a custom fill character and center-align. A
  quoted `${x:?}` rendering is KR-222 ([§2](#s2-quoted-format)); today `${x:?}` is a lexical error.
  Still open here: per-derive `@skip(Formattable)` / `@skip(Serializable)` for redaction (today `@skip` is one shared boolean —
  parameterize `FieldInfo::serSkip` to a per-derive set when a concrete case appears). Tagged-string *typed
  holes* (Model B) shipped at `0.9.512` as `Template<C>` (SPEC § *Tagged strings*). `html` returning a distinct
  `SafeHtml` instead of a `string` is **genuinely optional** for std: a tag may return any type, so a web
  framework that composes escaped fragments defines its own safe type, while std's `html` is the one-shot
  escaper. An **ORM in std is a NON-GOAL** (user, 2026-10-01): mapping a type to a row — fields to columns,
  generated `INSERT`/`SELECT`, rows decoded back — is a library's job above std, built from `@generate`'s field
  walk, the `SqlParam` contract and a driver's row decoding. std ships the parameter layer only. Regex is a separate campaign. `string + <number>` stays a compile error — a **non-goal**: `"${x}"` is the one way to render a value (SPEC § *Interpolation*), and the emitter's message says so.
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
  `-fno-sanitize=function`. Closed — kept here only so it is not re-diagnosed as a hole a third time. The one
  reason that would reopen it, noted 2026-10-30: the ruling weighed UBSan alone, and clang's CFI
  (`-fsanitize=cfi-icall`) would REFUSE these calls rather than report them, so a CFI build is where the typed thunk
  would earn its indirection.
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

- **Found by porting a game to kama** (2026-08-31). Six of these defects are now
  fixed: a multi-module library not being consumable as a dependency, the output being named after the
  alphabetically first source file, absolute dependency symlinks, three stale claims in the WebGPU
  example, and — in `0.9.128` — the two recorded below. They are kept together because how they were found
  is the point: every one was found by someone *using* the language rather than by the corpus, and not
  one of them had a fixture that could have caught it. ⚠️ **Two of the six turned out to be bigger than
  their report** (the `comptime` export was a whole unwired subsystem; the frame loop was a seam gap, not
  an example typo), which is the argument for probing a user's report rather than patching its sentence.

  - **A module-scope `comptime` constant could not be exported — FIXED 0.9.128.** Kept as a record
    because probing it found something much larger than the report, and the shape recurs. It was
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
    enough. The program reached 15.6 GB resident and took a machine down through the kernel watchdog,
    twice. ⚠️ **The language half is the larger point, and it promoted a row** — see the safe `std::gpu`
    wrapper in [§8](#s8). Nothing in kama could have caught this: the allocation is inside wgpu-native,
    reached through `UnsafePtr` handles carrying no RAII, so there is no kama object, no destructor and
    nothing for `@noheap` to see. ⚠️ **But be precise about which half would have caught it** — the
    original framing, and this entry's first draft, both said RAII, and that is wrong. RAII would not
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
    arguable half has an answer now: an engine built on kama **derives its own window** and
    duplicates ~60 lines of surface-derivation out of `kama_gpu.c` to get keyboard and mouse, which is
    the outcome a seam that throws its events away forces on everybody.

- **A second audit of kama in use (2026-09-01), triaged against this tree.** Everything below was
  REPRODUCED here before being scheduled — a report names the symptom, and three times running the
  shape underneath it has been different. Find an entry by its text, never by a remembered number.
  `@noheap` transitivity has also been verified by behaviour, on `0.9.132`, in a program built
  outside this tree.

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
      cost a bisection to find; it cost one line to fix.
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
    the CoreAudio thread, a synth runs on the frame loop and feeds the device through a ring — so
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
  - **Declared NOT ours** — the audio backend, WebGPU binding breadth, an engine's RFC6455
    framing, module statics being per-isolate (correct behaviour), and a PATH entry that is a directory
    breaking `make` in the emscripten image. Each belongs to the engine, not to kama.

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

- **The audio-seam cluster — 2026-09-01.** Ten gaps hit designing
  one audio device seam, recorded together because they are one story and because every citation was
  re-read against this tree before it was filed. The stake: the engine design that hit them says *"the
  platform split is one mechanism throughout — a `type contract` with `@compileFor`-gated
  implementations"*, and **that is not true today and cannot be made true** — the first two below are
  exactly why, and a shipped platform seam had to push its split down into C `#ifdef`s instead.
  ⚠️ **Every claim here was verified against the compiler, and two of the three "small wins" are
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

  - **SHIPPED `0.9.160`/`0.9.161` — no `-fsanitize` flag remains, on any target or tier.** The
    finding was that kama's unconditional `-fsanitize=integer-divide-by-zero,shift-exponent,
    float-cast-overflow` (with `-fsanitize-trap`, so no sanitizer runtime) made emscripten refuse
    `-sWASM_WORKERS`, and with it AudioWorklet — no audio thread in the browser at all — and that the
    refusal was over-broad for trap-only mode. The answer taken was the second of the three proposed,
    on every target rather than wasm alone: the four faults are now the compiler's own checks in the
    emitted C (`KAMA_DIV`/`MOD`/`SHL`/`SHR`, `kama_f2i_chk`, and in debug `KAMA_ADD`/`SUB`/`MUL`/`NEG`
    plus the place operators for `+=`/`++`), which cost the branch the sanitizer already cost, print a
    message where `ud2` printed nothing, run the panic hook, and are what an `@onPanic` region can recover
    from. Release codegen parity is asserted by `tools/check-release-arith.sh`. (Wasm Workers also need
    SharedArrayBuffer and therefore COOP/COEP headers — a hosting constraint, not kama's.)
- **The defects building a game on kama turned up, and what each one cost to find.** All
  reproduced against the shipped compiler before anything was written; two were fixed in `0.9.148` and
  `0.9.149`, one in `0.9.150`, `0.9.170`–`0.9.175` closed the raw-seam triage (three reported defects and
  three found here — the git log has each), and the residue below is what is left. The spurious import
  error (triaged 2026-09-04 against `0.9.164`) is the first open bullet.

  - **A module `static` cannot own a destructible resource — considered, deliberately DEFERRED, not a
    row (2026-09-04).** `static World g = World.make();` is refused, and the diagnostic states this stance.
    It surfaced as the ROOT of a game's calloc'd `World` (on the web `main`'s frame is unwound
    while the rAF callback lives), which is what made it look like the fix. It is not, and the reasoning
    is kept here so it is not re-derived. (The seam it was mistaken for — a local `UnsafePtr<T>` element
    is deliberately untyped to ownership so `nd[i] = od[i]` stays a bitwise relocate; widening `exprClass`
    fixes the reported case and breaks 45 fixtures — became DIAGNOSTICS in `0.9.174`, and then the audit found the
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
  - **A reported check/build split on `Mat4 * Vec4` is stale**: it is caught by `kama check` today ("the right-hand
    operand expects a `Mat4`, so it cannot be given a `Vec4`"), and check and build agree.

  - **SHIPPED `0.9.176`–`0.9.181` — the two wrong-file rows, and three more defects the first one was
    hiding.** Both ROADMAP rows are deleted; what follows is the record, then the original filing.
    - **The wrong-file import row was not a message bug.** The filing (and this section) described a spurious import
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

  - **A file must import a type it never names — the original filing.** Twelve lines, two files in one module:
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
    mechanism**: `site->line` is the field's line in `decl.kama` (7 in the repro, 5 with a value field)
    stamped with `main.kama`'s path — a 6-line file blamed at `7:0`. `run_tests.sh`'s
    `diag_position_faults` would flag it, in a `.d/` fixture that exercises it. Their cost statement is
    the one to keep: adding a typed field to a widely-held `type value` is a breaking change to every
    file that merely holds one, invisible from the type's own definition.

  - **That filing's RESIDUAL — a field PASSED ALONG still demanded the import — FIXED `0.9.189`.** Re-audited
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

  - **An `InlineArray` field's SIZE had to be imported with the type — FIXED `0.9.150`.**
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

  **⚠️ What these reports are worth reading for.** `Mat4 * Vec4` was filed as "ergonomic friction" and was a
  check/build divergence reaching four positions, not one; another was filed as an `InlineArray` problem and
  was `isConcreteTypeArg` not recognising a raw pointer, which broke EVERY generic inference over a
  pointer element; the field-size row was filed as an import problem and was a silent skip plus a size resolved
  in the wrong scope; the wrong-file import was filed as an import-rule problem and is that read-site resolution
  again, at three or more sites. **All four were bigger than the report, in the same direction: a
  user describes the shape they hit, not the rule that is wrong.** Re-derive the rule before sizing
  the fix. One report is the exception that proves it from the other side: filed as "emit a terminator after
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
    policy** was promoted by measurement rather than by argument — see the underrun numbers
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
  generic methods a method turbofish has nothing to name. Verdict (audit, 2026-09-07): a **non-goal** — a method's own type parameter would need a second turbofish grammar on a receiver call, and the free-function spelling above is the idiom. Reaffirmed by the maintainer 2026-09-30 when a database client asked for `row.get::<int32>(index: 0)`: beside `column::<int32>(row: r, index: 0)` it would be two ways to do one thing, and it buys a spelling, not a capability.

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
  shadow WORK while fixing a local-scope leak — a language decision taken inside a bug fix, in the wrong
  direction. `checkBinderShadow` now refuses a `for` counter, a `foreach` variable and a `match` payload
  binding that takes the name of a parameter, an enclosing-scope local or an in-scope field, with the
  same three messages the declarator gives. Sibling-scope reuse is untouched (`0.9.247`), which is the
  half of that fix that was a real bug. Six `tests/xfail/binder_shadow_*` fixtures, one per binder × message.
  ⚠️ **The corpus had exactly one instance and it is the argument for the rule**: `tests/net_addr_ctor`
  wrote `case V4(a: a, b: b, …)` while two `SocketAddr` locals named `a` and `b` were live — and the
  match SUBJECT was the outer `a`. It read as destructuring `a` into itself.

<a id="s4"></a>

## 4. Reflection + serialization — remaining follow-ups (1.x)

<a id="s4-json-comments"></a>

### A JSON reader that takes comments, for hand-written files (KR-339)

**Ruled 2026-11-07:** `kama.json` and `kama.local.json` stay strict JSON, so every JSON tool reads them, and a comment
there is refused naming its line (`0.9.867`); the docs' examples that showed `//` notes were corrected to match. The
other half of the report that led there is std's: a game engine on kama reads its hand-written records through
`JsonDeserializer`, and a designer annotating a record (why a tuning value is what it is) has nowhere to write it.
**Measured `0.9.860`:** a `//` line or a trailing comma in input to `deserializeJsonBuffer` fails "malformed input".

**What others do.** VS Code's settings and `tsconfig.json` are JSONC: `//` and `/* */` comments, and trailing commas
accepted in practice. JSON5 adds unquoted keys, single-quoted strings, hex numbers and more. Rust has separate crates
(`json5`, `jsonc-parser`); Go's `encoding/json` has none and `hujson` (Tailscale) normalizes JSONC to JSON first;
.NET's `System.Text.Json` takes `JsonCommentHandling.Skip` and `AllowTrailingCommas` as reader options, which is the
shape kama's reader already has for strictness.

**The shape, to design:** a constructor beside `make(src:)` and `strict(src:)` that also skips comments, so the READ
decides, as it does for unknown keys: one type read leniently from a hand-written file and strictly from a peer. The
open question is the dialect: comments only (the smallest, and all the report asks), or trailing commas too, which
hand-editing invites, or full JSON5. Writing keeps emitting strict JSON either way. Wanted by the owner beside the
`kama.json` ruling.

<a id="s4-fixed-array-serde"></a>

### A fixed-size array in a record (KR-342)

**Measured `0.9.871`:** a `@generate(Serializable, Deserializable)` record with an `InlineArray<Vec3>#(3)` field is refused at
the field, "`InlineArray` has none", in both directions, and `FixedArray<T, A>` writes a JSON array but has no `deserialize`
(its own note: the length is unknown until read, and moving a temporary's buffer in needs a buffer-steal ctor). Found
deriving serde for `std::math` (`0.9.872`): the vectors and `Quat` derive, but `Mat2/3/4` hold their columns in an
`InlineArray` and cannot, and neither can any record holding a fixed array, such as KR-329's `Clip { InlineArray<float32>#(64)
keys }`. `InlineDynamicArray<T>#(N)` already reads and writes, failing `TooMany` past `N` (since `0.9.856`).

**The shape:** an `InlineArray<T>#(N)` is a JSON array of exactly `N`, written and read element by element through `T`'s
own serde, so a matrix reads as `{"cols": [{"x":1,…}, …]}`. `InlineArray` is a compiler intrinsic with no kama body, so the
compiler writes its serde per instantiation, as it writes a record's. **The design question:** what a short array reports.
`TooMany` exists for a long one; a short one is either a new kind (`TooFew`, "expected 3 elements at `m.cols`, got 2"), or a
`MissingField` naming the first absent index (`m.cols[2]`), which needs no new kind and says where. And whether a merge
may write a prefix. `FixedArray`'s read lands with it, since the length question is the same.

<a id="s4-view-deserialize"></a>

### Zero-copy deserialization into a view struct (KR-231)

**Ruled 2026-10-22.** A type that holds a view is a view since `0.9.725` (SPEC *Views compose*), so
`@generate(Deserializable)` can target a `type view` record whose `ConstView<uint8>` fields point into the input.
Parsing JSON then copies no string, as serde's `#[serde(borrow)]` does in Rust. The record is bounded by the window
over the input, under every existing view rule. A field that needs unescaping (`\n`, `\u00e9`) cannot point into the
input. The design must say what happens there: Rust uses `Cow<str>`, and the alternatives are refusing the record or
choosing a per-field owned fallback. Unscoped; nothing blocks it.

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

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T>#(N)` read, now its own row, KR-342; a bare `serializeJsonBuffer`/`deserializeJsonBuffer` of an
  intrinsic value. (A `const` field is a separate general language gap — doesn't parse today.)
  ✅ The enum half of this bullet SHIPPED with the derives: a bare `serializeJsonBuffer`/`deserializeJsonBuffer` of an enum value works
  (the cause was the missing `<Enum>__as_Serializable` vtbl, not a missing wire form), and so do
  generic enums, per instantiation. See *Derive follow-ons* in §2.
- **Binary backend follow-on.** Delta/snapshot replication stays ENGINE-level (above serde); generic byte
  compression is an io-adapter layer (§1 transform adapters), not a serde concern. The schema-locked
  positional mode is no longer deferred: it is the *Positional binary backend* row, built on the design below.
- **Field addressing — SHIPPED `0.9.257`–`0.9.262`.** Where it came from: a six-field
  frame measured 24 bytes of data and 91 on the wire under KBIN, because every object carries every field NAME. A
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
  - **Restricted-private contract members are a NON-GOAL** (decided 2026-10-16, shipped `0.9.646`). A contract
    member is public by definition and takes no visibility word; who may call it is said by a parameter only that
    caller can build (a key type whose `ctor` it alone is granted). SPEC § *Contracts* records it beside the refusal.
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
  impl + its own `serialize<Format>Buffer`/`deserialize<Format>Buffer` pair. (`std::encoding::base64` shipped `0.9.197` as its own small module, with
  `::hex` beside it — SPEC § *Encoding*.)
- **A back end's ENTRY POINTS are a convention, not a contract** — the defect the line above quietly
  describes. `Serializer`/`Deserializer`/`Serializable`/`Deserializable` are real contracts
  ([prelude/global.kama:207](../prelude/global.kama)), but each back end's `serialize…Buffer`/`deserialize…Buffer`/`…Stream` are **bare free
  functions**, duplicated per back end (`text/json/json.kama`, `binary/kbin/*.kama`) with nothing
  checking that a back end supplies them or that their signatures agree. "A drop-in twin of the JSON back
  end" is true only by discipline. Wants a `Format` (or `Codec`) contract carrying the three, so a back end
  is a checked implementation. It is also the source of the **one** name collision in the flattened-stdlib
  measurement (json vs binary, before `0.9.265` put the format in every entry-point name). Take it with the std-lib cleanup pass, not before.
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

### The allocation campaign — opened 2026-09-12, COMPLETE 2026-09-20

What it did, in one paragraph: `kama_alloc`/`kama_free` became the only way heap memory is obtained or
released, delegating to a global allocator that defaults to `malloc`/`free` and that a program can
replace; every raw site in emitted C, the runtime headers, the OS seam and the prelude moved onto them,
which also ended the ~20 blocks allocated by one family and freed by another (correct before only because
every family was libc); error boxing draws from an allocator like every other box; and `--no-heap` judges
what the program REACHES, consistently, rather than every imported body. A provable callee behind a
contract slot was left open, turned from theoretical into measured — see the serde note under *Non-goals*
below — and shipped in `0.9.659`: a slot is an edge to every implementer the program defines (SPEC
*No-heap subset*).

⚠️ **`docs/design/allocation.md` was DELETED when the last row shipped** (`0.9.412`), by the maintenance
rule for `docs/design/`. This section and SPEC are its record; the reasoning behind each step is in the
git log, and the verdicts it was the only home for are kept below rather than lost with it.

**Step 1 shipped** (`0.9.347`–`0.9.348`). `--no-heap` judges what the entry points reach, over a call graph
read from the emitted C, and C the compiler cannot read declares itself with `@heap extern fn`. Its record is
SPEC *No-heap subset*, docs/targets.md (the section-GC contract).

**Step 2 shipped** (`0.9.366`–`0.9.369`). One funnel, `kama_alloc(n, align)`/`kama_free(p, n, align)`,
carrying each block's LAYOUT because the `Allocator` contract promises it (`allocate(bytes, align)`/`deallocate(pointer,
bytes, align)`); `tools/check-alloc-funnel.sh` keeps the C allocator inside it, and the san leg's `KAMA_ALLOC_CHECK`
proves every release hands back its exact layout. The record is SPEC (*Allocator*, `sizeof(ptr:)`/`alignof(ptr:)`).
Verified on Linux, wasm and Windows (2026-09-17: the Windows branch of `kama_os.h` needed no
change; two guards did, for msys2's gawk and its missing python3).

**Step 3 shipped** (`0.9.377`–`0.9.378`). `@globalAllocator type resource Pool implements GlobalHeap`
replaces the funnel's default: one instance, `= {0}` in the entry TU, shared by every isolate. With a declaration
every funnel fact in the no-heap walk is an edge into the pool, so its body decides. `Shared.adopt` and the
`SortedMap` root now draw from `A`, and a bare `new` needs `A == GlobalAllocator`. The record is SPEC *Global
allocator*.

**Reaching the instance — SHIPPED `0.9.411`.** `globalHeap::<Pool>()` gives the one instance as a place
(`ref Pool`), checked against the declared `@globalAllocator`. The record is SPEC *Global allocator* (*Reaching the
instance*); `tests/global_allocator_reach{,_units}.kama` assert a live count directly, which
is what the fixtures had to prove by exhaustion before it.

Three things it turned up, none of them in the plan it was picked up with:
- **The forward declaration is not a multi-TU concern.** The instance is emitted where the ENTRY function is, i.e.
  AFTER every body in that TU — in the emitted C of `tests/global_allocator_serde.kama` the definition sat at line
  7497, below `kama_main` at 7422 — so nothing could name it, the declaring file included. One `extern` in
  `emitHeaderContent` covers both builds, because the single-file path inlines that content into the same stream.
- **`invocationReturnsPlace` resolves a bare call through `_funcs`**, so an intrinsic with no entry there answers
  FALSE and `ref Pool` would not bind: the place would be copied, which for a `resource` is refused. Three sites
  had to agree, not one.
- **The escape rule refused the intrinsic and a plain module `static` by the same line, for the same wrong reason.**
  The maintainer's call (2026-09-20) was one predicate over both rather than a carve-out for the builtin: static
  storage outlives every frame unconditionally, so it is the least conditional of the three roots, and every
  comparable language treats it as the easy case (Rust's `&'static`, a Meyers singleton, a Go package-level var).
  `static Counters g; fn ref Counters counters() { return g; }` is now legal for everyone
  (`tests/reffn_static.kama`); a local that merely shares a static's name is still refused
  (`tests/xfail/reffn_static_local.kama`), which is why the root resolves as a SYMBOL and not as a bare name.

**The Windows path seam shipped** (`0.9.416`), and closed the campaign. `kama__wpath` converts into a
caller-owned `kama__wpathbuf` on the stack, so the ten `std::fs` path externs allocate on no target and a
`--no-heap` program may `stat`, `rename`, `remove`, `createDir`, `removeDir` and `File.open`. The record
is SPEC *No-heap subset*; the measurements and the ⚠️ `KAMA_NOINLINE` stack rule are in
[docs/platforms/windows.md](platforms/windows.md), because they are Windows facts rather than language
ones. `readDir`, `readFile`, `createDirAll` and `removeDirAll` stay heap on every target, for reasons of
their own, and `tests/xfail/noheap_flag_fs_readdir.d` pins that boundary.

**Non-goals the campaign settled** — kept here because the design doc that decided them is gone:

- **A per-call error allocator is a NON-GOAL.** The question was whether `Serializable`/`Deserializable`
  should carry `Result<T, Owned<Error, A>>` so a caller can put errors in the same arena as the values.
  It should not: the error box already follows the program's DECLARED allocator, so the case this serves
  is narrower than it looks — a program wanting its errors in a different arena from its global pool,
  when `@globalAllocator` exists precisely so one pool answers for everything. Against that, the contract
  change is source-breaking for every hand-written serde in every consumer, and it puts an allocator
  parameter in the signature of the most-implemented contract kama has. If a per-arena error is ever
  wanted, it is wanted for fallible APIs in general and belongs to a design for those, not to serde.
  What that left unproven shipped in `0.9.659`: dropping a boxed error dispatches a destructor
  through a contract, and the walk now resolves the slot to every implementer's destructor, so the same
  serde program builds under `--no-heap` with a pool (`tests/noheap_pool_serde.d`).
- **A per-isolate override of the DEFAULT allocator is a NON-GOAL.** What it would redirect is exactly
  what crosses isolates by construction — a spawn bundle allocated in the parent is freed in the child,
  strings and boxes travel over channels, a channel's ring is shared by both ends — so each would be
  freed into the wrong allocator, which is the hazard the singleton exists to prevent; and it would make
  allocation implicit (GOALS #5). What remains outside explicit per-isolate allocation is what has no
  `A` at all (`string`, `Formatter`, `Owned<Error>`, runtime internals); if that ever matters, the answer
  is giving those an `A`, not an implicit override.

- **Reflection + declarative serialization** — see §4; back ends follow as modules. Rides on the shipped
  `std::fs`/`std::io` for asset + scene load.
- **Container / data-structure reach — honest non-goals.** The core containers ship (`DynamicArray`/`FixedArray`/
  `InlineArray`/`string`, `Map`/`Set`, `Deque`, `PriorityQueue`, `SlotMap`, `BitSet`, `SortedMap`/`SortedSet`,
  `OrderedMap`/`OrderedSet`, `View<T>` — see SPEC). Shipped `0.9.689`: a bounded inline vector, `InlineDynamicArray<T>#(N)`. **Not** planned as stdlib: general **linked lists** (mostly a cache anti-pattern in
  data-oriented engines — the useful form is an intrusive free-list / LRU); raw **BSTs** (subsumed by the sorted
  map); **spatial trees** (quadtree/octree/BVH/k-d — engine-specific); **a multimap or multiset** (decided
  2026-10-16: `Map<K, DynamicArray<V>>` answers it, and only C++ ships one); **an LRU cache** (decided 2026-10-16:
  `OrderedMap` plus its move-to-end is the whole of it, Java's `LinkedHashMap` precedent). A concurrent map is §6's
  question, not this list's.

<a id="s5-mcu-fibers"></a>

### Fibers on bare metal (KR-233)

**Ruled 2026-10-22, as KR-3's decision 15.** A microcontroller has no `Poller`. A fiber there needs:
- a Cortex-M switch, about ten instructions;
- stacks drawn from a static pool, sized at build time, so `--no-heap` holds;
- wakes from an interrupt handler through an ISR-safe ready queue.

Parking already allocates nothing, because the waiter record lives on the fiber's stack. This comes after KR-3's
native scheduler, beside KR-19's MCU packaging, and wants hardware to verify on.

<a id="s5-bare-metal-concurrent"></a>

### A bare-metal build cannot import `std::concurrent::Atomic` (KR-341)

Found 2026-11-07, building `Atomic.wait`'s bare-metal branch (KR-3 step 2, W5). This program:

```kama
import { std::concurrent::Atomic };
fn uint8 main() {
    Atomic<uint32> ready = Atomic.make(value: 1);
    ready.notifyAll();
    return cast<uint8>(ready.wait(whileEquals: 0));
}
```

It does not build with `--target embedded --select OUTPUT=STATIC --cc "clang --target=armv7m-none-eabi"`, which is how
`tools/check-embedded.sh` stands in for a firmware toolchain. Two of the units it compiles include libc headers that a
bare-metal compiler without a libc does not have:
- `std::concurrent`'s `Isolate` unit (`concurrent.kama`), whose `kama_isolate.h` includes `<pthread.h>`. It was compiled
  for an `Atomic`-only program before `Atomic.wait` too, since its destructor is kept. With the host's headers the
  archive references `pthread_join` and `free`.
- `std::time`'s units, whose `kama_time.h` includes `<time.h>`. They are reached since `0.9.871`, because `waitUntil`
  takes an `Instant`.

With a glibc host's compiler and headers in place of the triple, the build fails as well. The `Isolate` unit's `cpuCount`
branch is chosen by `__linux__`, not by the target, and includes `<stdio.h>`. glibc's `sscanf` carries an asm label
there, which clashes with the embedded runtime's own declaration of it ("cannot apply asm label to function after its
first use", measured on the Linux leg).

`kama_atomic.h` calls itself "freestanding-friendly" and `kama_locked.h` has a branch for `--target embedded`, but no
fixture or guard ever built a bare-metal program importing `std::concurrent`. Until this is fixed, the `Atomic.wait`
check in `check-embedded.sh` compiles `kama_wait.h` on its own with the embedded flags and the host's headers. So it
proves the header's branch, which references no symbol but the clock's, and not the toolchain. The fix wants a bare-metal shape for the module's thread machinery: refuse `spawn` there, and compile none
of it. It also wants a clock on bare metal, which is KR-19's: a board supplies `kama_now_mono_ns`, as it supplies
`kama_panic_handler`.

<a id="s5-isr-shared-cell"></a>

### `main` and an interrupt handler share a cell only through `unsafe` (KR-340)

Filed 2026-11-07, while ruling `Atomic.wait` (KR-3 step 2, W5). An `@interrupt` handler takes no parameters, so whatever it
shares with `main` is a module `static`, and on bare metal a `static` is one plain C global. A `static` holds no
`resource`, though, and `Atomic<T>` is one, so the safe spelling is refused: *"`Atomic<bool>` is a `resource`, though it owns
nothing"*. Through `0.9.869` that message said "owns memory", which was false. What compiles today is
`static UnsafePtr<Atomic<bool>>`, set from `main` in an `unsafe fn` and read the same way in the handler (probed
`--target embedded`). A plain `static bool` is accepted, and C may hoist its load out of `main`'s loop, so a flag the handler
sets is never seen.

| | an interrupt handler's flag |
|---|---|
| C | `volatile sig_atomic_t`, or a C11 `_Atomic` global |
| Rust | a `static AtomicBool`, which needs no `unsafe` since an atomic is `Sync` |
| Zig | a global `std.atomic.Value(bool)` |

On a hosted target a `static` is per isolate (`_Thread_local`), and a foreign thread's callback reaches a cell through the
pointer its C API hands it, so the case is bare metal, and a signal handler on the thread that installed it. **Verdict:
scheduled, design first, with KR-233**, whose fibers wake from an interrupt handler through the same kind of cell. The
question to rule is whether `Atomic<T>` alone is admitted as a `static`, or whether a `static` gains an atomic kind of its
own. `Atomic<T>` owns nothing and has no teardown. What admitting it would still need is a rule that nothing moves it out
of the `static`; no `static` has held a `resource` yet, so that rule has never been needed or probed.

<a id="s5-file-watch"></a>

### No file watcher in std (KR-330)

Raised 2026-11-05 by a game engine on kama, for a running game that reloads its project data in development, and by the maintainer: *"File watcher is
important."* Nothing in `lib/std` or `include/` binds a change notification, and `std::net`'s `Poller` refuses a regular file on Linux (`poll.kama`), so a
program can only poll a file's modification time. KR-24's hot-reload library names file-watching as one of its three parts.

**What others do.**

| | in the standard library | Linux | macOS | Windows | recursive |
|---|---|---|---|---|---|
| Java | `WatchService` (JDK 7) | inotify | a polling fallback, still in 2022 ([OpenJDK discuss](https://mail.openjdk.org/pipermail/discuss/2022-August/006144.html)) | `ReadDirectoryChangesW` | per directory |
| .NET | `FileSystemWatcher` | inotify | FSEvents | `ReadDirectoryChangesW` | `IncludeSubdirectories` |
| Node | `fs.watch` | inotify | FSEvents | `ReadDirectoryChangesW` | Linux since 19.1 |
| Deno | `Deno.watchFs` | through Rust's `notify` | | | yes |
| Dart | `FileSystemEntity.watch` | inotify | FSEvents | `ReadDirectoryChangesW` | not on Linux, its 2.x docs say |
| Rust | no: the `notify` crate (rust-analyzer, cargo-watch, Deno) | inotify | FSEvents or kqueue | `ReadDirectoryChangesW` | yes |
| Go | no: `fsnotify` | inotify | kqueue | `ReadDirectoryChangesW` | no |
| Python | no: `watchdog` | inotify | FSEvents | `ReadDirectoryChangesW` | yes |
| Zig | no general API; `std.Build.Watch` drives `zig build --watch` | fanotify | kqueue, then FSEventStream | since 2024 | |

**What every one of them meets.** A kernel queue overflows (inotify's `IN_Q_OVERFLOW`, `ReadDirectoryChangesW`'s `ERROR_NOTIFY_ENUM_DIR`, .NET's
`InternalBufferSize`), and the answer is an event saying "rescan", never silence. An editor saves by writing a temporary file and renaming it over the
original, so a watch on the file sees it vanish. inotify watches one directory per descriptor, bounded by `max_user_watches`, so recursion is a walk
kept in step with creations; FSEvents and `ReadDirectoryChangesW` recurse natively. Events arrive in bursts and want coalescing.

**Where it fits kama.** An inotify descriptor is pollable, so on Linux a watcher is one more source for the `Poller` each fiber scheduler owns, and a fiber
parks on it like a socket. On Windows a directory handle's overlapped `ReadDirectoryChangesW` completes through the completion port that KR-3's step 8
(this box's) builds. On macOS FSEvents delivers on a dispatch queue, which would wake the `Poller` through a pipe; kqueue's `EVFILT_VNODE` needs a
descriptor per file. wasm has no file system to watch in a browser. **Verdict: scheduled, design first:** the API (a watcher over paths, recursive or not,
yielding events with the overflow and rename cases stated), its delivery through the `Poller`, and the platform order, Linux and Windows first.

**The recommendation, for when it is designed (2026-11-05, production grade, unbuilt).**
1. **One resource, `std::fs::Watcher`, blocking-shaped like a socket.** `watch(path, recursive:)` adds a root and
   `unwatch` drops one; `next()` hands back the next event and parks a fiber (a thread with no scheduler blocks). No
   callback and no channel in std's surface: a program that wants one wraps `next()` in a fiber.
2. **Events: `Created`, `Modified`, `Removed`, `Renamed(from, to)` and `Rescan(root)`,** each with a path relative to its
   root. `Renamed` is paired where the OS pairs it (inotify's cookie, `ReadDirectoryChangesW`'s old and new names) and
   arrives as `Removed` and `Created` where it does not. **`Rescan` is the overflow:** the kernel dropped events and the
   root must be read again. Never silence, which is the bug Java's macOS fallback and every unchecked queue ship.
3. **A file is watched through its directory, filtered to its name,** so an editor's save-by-rename keeps reporting:
   .NET's `Filter` and fsnotify's own advice.
4. **Recursion where the OS has it** (`ReadDirectoryChangesW`'s subtree flag, FSEvents), and on Linux a walk that adds an
   inotify watch per directory and adds each new directory as it appears, reporting what it holds as `Created` to close
   the race. Running out of `max_user_watches` is an error naming the setting, not a quiet partial watch. fanotify needs
   `CAP_SYS_ADMIN`, so not it.
5. **No silent polling fallback.** A file system the OS cannot watch (a network mount, say) is an error; Java's polling
   on macOS is the lesson. An explicit polling watcher can come later, by name.
6. **No debouncing in std.** Events come as the OS coalesces them; Rust's `notify` keeps its debouncers in separate
   crates for the same reason. A debounce is a few lines over `next()` and a timer, and is the caller's policy.
7. **Delivery and platforms:** inotify's descriptor in the `Poller` (Linux); a directory handle's overlapped
   `ReadDirectoryChangesW` on KR-3's step 8 completion port (Windows, this box); FSEvents on a dispatch queue waking the
   `Poller` through a pipe (macOS, on the Mac); kqueue with KR-227's leg (FreeBSD); refused at compile time on wasm. Linux
   and Windows first, since the suite runs both here.

<a id="s5-repro-math-cr"></a>

### `reproducible-float`'s math, correctly rounded (KR-335)

Under the key, `std::math`'s 14 functions that IEEE 754 does not require correctly rounded come from musl 1.2.6 compiled into
the program (`include/kama_libm.h`, generated by `tools/gen-libm`; targets.md has what the key covers and costs). That gives
the same bits on every target, but not the correct ones, and so the bits are only as fixed as the vendored version: a musl
bump that changes a function changes what every replay recorded before it computes.

**The next step, ruled 2026-11-05 as "later, once measured": CORE-MATH** (MIT), correctly rounded throughout. A correctly
rounded answer is unique, so it is the same on every target by definition, it cannot change when the library is bumped, and
each platform could keep a fast FMA path and still agree. What is known without running it: it changes wasm's bits too (to
the correct ones); it leans on `fma`, which baseline x86-64 and wasm compute in software; glibc and CORE-MATH have been
converging on correctly rounded `float32` functions. It was downloaded on 2026-11-05 and not built: compiling and running
downloaded source needs the maintainer's leave. Measure it on wasm before anything else, against musl, per function.

**The fixture changes with it.** `tests/repro_math.d` holds the vendored copy to emscripten's own musl (its expected hashes
come from a C probe built for wasm, independent of kama), so a correctly rounded library needs a new oracle for the same
inputs: MPFR, or CORE-MATH's own results compared across two targets.

**The alternative, for speed only: swap single functions.** Measured on a quiet x86-64 Windows box (`0.9.857`, ns per call,
musl against UCRT with FMA3), the 28 sum to about the same, but musl's `log10` takes 1.9 times as long (`log10f` 2.4), `powf`
1.8 and `tan` 1.4, while its `cbrt` runs twice as fast and `hypotf` 3.7 times. Arm's optimized-routines (where musl's `exp`,
`log` and `pow` already come from) has faster versions of some; a swap changes the bits on every target at once, which the
key permits as long as every target compiles the same copy. Unmeasured.

<a id="s5-wasm-closure"></a>

### A wasm release build does not run Closure (KR-289)

Reported 2026-11-02 (emscripten 6.0.13); the flag's absence checked on `0.9.805`, the sizes not re-measured here. A
`--release` wasm build passes `-Oz` and section GC (`kama.driver.cpp`, the release block) and never `--closure=1`,
while emdawnwebgpu's README says to enable Closure in every release. On `examples/webgpu` (brotli -q 11) it was
reported to take the JS from 11,768 to 7,117 bytes and the whole build from 22,518 to 17,867, drawing the same pixels.

What a default needs: std's own `jsLibraries` Closure-safe (Closure renames unquoted property names), the rule
written in docs/targets.md for a project's `jsLibraries`, and a way out for a library that is not. A project opts in
today with an `ldflags` entry gated to `ARCH_WASM32`. Which Closure emsdk runs on each host (a native binary or the
JVM) is to check.

<a id="s5-wasm-opt"></a>

### A wasm release build is optimized for size, and nothing picks speed (KR-332)

Found 2026-11-05 while measuring `reproducible-float`'s math. `--release` compiles native at `-O3` and wasm at `-Oz` (GOALS.md; the driver's comment says
"download cost dominates"). At `-Oz` emscripten links `libc_optz`, whose `pow`, `log` and `log2` are table-free versions kept for size.
Compiled natively they run 2.7× (`pow`), 1.5× (`log`) and 1.6× (`log2`) slower than Arm's; on wasm, unmeasured. A debug wasm build
(`-g -O0`) links the fast ones, so on wasm `pow` is faster in a debug build than in a release build. The rest of the program pays `-Oz`'s
usual price too (less inlining, no unrolling), unmeasured for kama. Whether a raw `cflags` or `ldflags` entry gated to `ARCH_WASM32` can
override it is unchecked: emscripten picks `libc_optz` from the link line.

The maintainer's lean (2026-11-05): performance first, since a game downloads once and runs for hours. Measure before choosing:
`examples/webgpu` and a simulation-shaped bench at `-O2`, `-O3`, `-Os` and `-Oz`, compressed size against speed. Then decide between a
different default, a manifest setting, or `-Oz` kept with the reason written down. `reproducible-float`'s math functions do not depend on
it: under the key, musl's copy is compiled into every build (`0.9.857`).

<a id="s5-android"></a>

### An installable Android app, for Meta Quest's native route (KR-283)

**The brief for the session that builds it, on the Windows box (it has the headset).** The owner, 2026-11-01: a game
engine on kama will ship on Meta Quest both through the browser (WebXR) and natively; XR itself is the engine's (§8).
Quest is an Android device (Horizon OS is Android 14, SDK 34), and its native XR is OpenXR. The Android target shipped
in `0.9.830` (docs/targets.md): `--target aarch64-linux-android29` builds with the NDK kama finds itself, and `./dev
test android` runs the suite over adb on the device's own CPU. What is left is the app the launcher starts. The SDK's
emulator carries everything up to the VR launch itself, so the work does not wait on the headset; nor on the PC's GPU,
since a Quest-native app renders on the headset.

**What the device leg leaves to the app** (measured 2026-11-03 on the emulator, AVD `kama34`, API 34):
- **14 fixtures skip.** `adb shell`'s SELinux domain (`u:r:shell:s0`) may create no socket or FIFO file in
  `/data/local/tmp`, though an app may in its own directory, so the leg skips the fixtures that make one (a
  `std::net::Unix…` socket or `mkfifo`, `run_tests.sh`'s `SET_FILE_NODE`). An app's run is where they are measured.
- **3 fixtures are unsettled on aarch64.** `proc_detach_dtor`, `proc_fiber_pipe` and `proc_open_file_limit` fail
  as aarch64 under the emulator's ARM translator (`ndk_translation`) and pass as x86_64, so the leg builds for the
  device's own CPU. The Quest is a real aarch64 device and settles them: `./dev test android` with the headset attached.

**What KR-283 builds.**
- **An app.** Quest launches an APK whose activity is `android.app.NativeActivity`. It loads `lib<name>.so` and calls
  `android_main` through the NDK's `sources/android/native_app_glue`. The entry needs no compiler change (built
  2026-11-02): `@linkName("android_main") expose unsafe fn void androidMain(UnsafePtr app)` exports `android_main` from
  the `.so` (`unsafe`, since it takes a raw pointer), and the glue is a `csources` entry. The OS owns the loop, as the
  browser does on wasm: events arrive through `ALooper_pollOnce`, the window through `APP_CMD_INIT_WINDOW` and
  `APP_CMD_TERM_WINDOW`, plus pause and resume.
- **Packaging.** An `AndroidManifest.xml` with Meta's required entries, compiled by `aapt2` against the platform's
  `android.jar`. Then the `.so` at `lib/arm64-v8a/`, `zipalign`, and `apksigner` (v2 signing, which store review
  requires). Every tool is in the Android SDK, and `apksigner` needs a JDK. Meta's release-manifest page, binding
  through VRC.Quest.Packaging.1 (read 2026-11-03,
  <https://developers.meta.com/horizon/resources/publish-mobile-manifest/>), requires: the `uses-feature`
  `android.hardware.vr.headtracking` (`required="true"`, `version="1"`) for an immersive app; a launch activity with
  `MAIN`, the category `com.oculus.intent.category.VR` (Meta's OpenXR page shows Khronos's
  `org.khronos.openxr.intent.category.IMMERSIVE_HMD` instead, and Godot's Meta plugin emits both) and
  `excludeFromRecents`; the meta-data `com.oculus.supportedDevices` (`quest2|questpro|quest3|quest3s|vrglasses`);
  `installLocation="auto"`; no `debuggable`; targetSdk 34 for an app created since 2026-03-01, minSdk 29 to 34 (32
  recommended); and a network security configuration, whose attribute the page does not name. Meta's native page adds
  `screenOrientation="landscape"` and a `configChanges` list, and is older than the release page (it shows 32/32).
  `com.oculus.vr.focusaware` is on neither page, though upload checks rejected apps without it through 2025.
- **Output.** Under `adb shell` a program has a stdout; an app the launcher starts has none, so `println` goes nowhere
  unless something routes it to logcat (`__android_log_write`, `-llog`).

**What others do.**
- **Rust:** cargo builds the `.so` (`cargo-ndk` wires the NDK linker), and the `android-activity` crate is the
  NativeActivity/GameActivity glue. Packaging was `cargo-apk` (deprecated, last release 2023-11) and then `xbuild`,
  whose repository now says it is unmaintained. Rust names the OS `target_os = "android"`, apart from Linux.
- **Go:** `gomobile build -target=android` packages and signs an APK itself, writing binary XML without the SDK's
  tools. `GOOS=android` also satisfies the `linux` build tag, which is kama's `OS_LINUX` + `ABI_ANDROID` shape.
- **Zig:** `zig cc` has no bionic, so it uses the NDK's sysroot; packaging is community templates.
- **C and C++:** Gradle with CMake or ndk-build, or aapt2 and apksigner driven by hand. A Gradle-free Quest 3
  OpenXR sample exists (CMU's 15-466 `ndk-openxr`).

**How an app's packaging is configured** (surveyed 2026-11-03 from each tool's docs or source):

| | app id | version code | manifest | release signing |
|---|---|---|---|---|
| cargo-apk | `package`, default `rust.<name>` | derived from the Cargo version (bit-packed) | generated from TOML tables | path and password in Cargo.toml, or env |
| xbuild | default `com.example.<name>`; iOS gets the bare name | derived from semver | a YAML mirror of the XML | a PEM key, `--pem` or env |
| gomobile | invented, `org.golang.todo.<lib>` (a TODO in its source); `-bundleid` is iOS-only | fixed at 1 | the package's own file as-is, else generated | none: one hardcoded debug key |
| Tauri v2 | `identifier`, one for both platforms; its default is refused | `major*1000000 + minor*1000 + patch`, overridable | a generated Gradle project you edit | `keystore.properties`, outside version control |
| Briefcase | `bundle` + the app's name, both platforms | version plus a build number, padded | generated, plus raw XML fragments | by hand (`jarsigner`) |
| Android Gradle | `applicationId` | independent integer | merged from every library, with markers | `signingConfigs`, passwords in a properties file |
| Godot 4 | `package/unique_name` | independent integer | generated, plugins splice entries | env (`GODOT_ANDROID_KEYSTORE_RELEASE_*`) or a credentials file |

What the table says: the tools that build iOS and Android from one config take **one** reverse-DNS id (Tauri,
Briefcase), and the per-platform tools drift (xbuild's two defaults, gomobile's invented one). A tool that defaults the
id ships a placeholder, and Tauri refuses its own. Every tool takes **one** version string and derives the integer by
default, each by a different formula. No mature tool keeps a password in the committed config. The debug key is
`~/.android/debug.keystore` in Android Studio and cargo-apk, so their APKs install over each other; xbuild and
gomobile ship a fixed key. Three manifest strategies: structured keys, raw fragments spliced in, or a whole file.

**The first step, `kama.json`'s shape — ruled 2026-11-04: all seven as recommended (2026-11-03), with 3's open
question answered.** Built in `0.9.873` after the hand proof below, as written here but for `vr`'s value, a list
(`["quest3", "quest3s"]`) rather than the `|`-joined string the manifest carries.
1. **`"kind": "app"`** says the project is an app the OS launches, beside `executable` and `library`, with `entry`
   naming the file whose `main` runs. On Android an app builds the `.so` with the glue and the APK around it; on a
   target with no app format it builds as an executable, an accepted no-op as `subsystem` is. An `executable` stays
   what `adb shell` and the device leg run. The alternative, the `app` block's presence switching the output, is
   implicit; `-o x.apk` is a second place to say it.
2. **`"app": { "id": "com.example.game", "label": "Game" }`**, the identity iOS will share (KR-281). `id` has no
   default: an app without one is refused, naming the key. Its segments are a letter and then letters and digits,
   two or more, which is what both platforms accept (Android allows `_`, Apple `-`; to confirm Apple's rule on the
   Mac). `label` defaults to `name`.
3. **The version is `version`.** The version name is the manifest's semver as written. The version code is derived as
   Tauri derives it, `major*1000000 + minor*1000 + patch`, with no override, refused past Android's 2100000000 or a
   part past 999. **An app's version carries no pre-release tag and no `+build`** (ruled 2026-11-04), refused with
   the patch bump that replaces it: `1.0.0-beta.2` would share `1.0.0`'s code, which a store refuses on the second
   upload, and Apple's App Store takes only up to three dot-separated integers (its ITMS-90060 error), so the shared
   version could never carry one to iOS. Android itself allows one, since its version name is a free label, and the
   tools that derive the code drop it without a word (cargo-apk gives `254.254.254-alpha.fix+2` the code of
   `254.254.254`); Flutter instead types the code by hand (`1.2.3+4`). A beta is a patch, sent through the store's
   own test channel. A library's `version` keeps semver's pre-releases for the registry.
4. **minSdk is the triple's level** (`aarch64-linux-android29` is minSdk 29): it is one number already, and a second
   spelling could disagree. **`app.android.targetSdk` is stated, with no default**: Android gates behaviour changes
   on it, so it is the project's choice. cargo-apk and xbuild default it (35 and 33), and neither is the 34 Quest
   requires today. It also names the `android.jar` aapt2 compiles against, and a missing platform is refused with the
   `sdkmanager` line that installs it.
5. **The manifest is generated, or supplied whole.** Generated from `app` and a closed set of keys under
   `app.android`: `permissions` (a list), and `vr`, which emits Meta's required set above with `supportedDevices` as
   its value. `"manifest": "<path>"` replaces it whole (gomobile's way), checked only where kama must agree with it:
   the id, the activity, `lib_name` and minSdk. No merging, no fragments. A dependency's `permissions` union in,
   since the package that binds an API is the one that knows its permission (as with `link`). Everything else in
   `app` is the root's alone, like `shellFile`.
6. **Signing follows the mode.** A debug build signs with `~/.android/debug.keystore`, which kama creates with
   `keytool` when it is missing, using Android Studio's parameters. A `--release` build signs with
   `app.android.keystore: { "path", "alias" }`, and is refused without it. Passwords come from the environment
   (`KAMA_KEYSTORE_PASSWORD`, `KAMA_KEY_PASSWORD`), and a password in `kama.json` is refused.
7. **Out of the first step:** `assets` (the APK's `assets/`, which needs a std reader over `AAssetManager`), `kama run`
   installing and launching the app over adb, and the lifecycle (decision 4 above).

**The hand proof (2026-11-07; emulator `kama34`, API 34; build-tools 37.0.0, NDK r30, Temurin 21).** A `.so` from
`kama build --shared`, with the glue as a `csources` entry and an exposed `android_main`, then by hand: `aapt2 link -I
android.jar --manifest`, the library added, `zipalign -P 16 -f 4`, `apksigner sign`. It installed, launched, logged and
painted its window, and so did an arm64-only APK through the emulator's ARM translator (`primaryCpuAbi=arm64-v8a`).
What it found:
- **aapt2 adds no file, and the SDK's legacy `aapt add` deflates the library** whatever `-0 so` says, while
  `extractNativeLibs="false"` (AGP's default since minSdk 23) needs it stored and page-aligned. The JDK's `jar --update
  --no-compress --no-manifest` stores it, and apksigner already needs that JDK. Writing the zip entry in kama was the
  other answer, and it is a rewrite of a tool the toolchain already has.
- **apksigner signs v2 and v3 by default**, and `verify` reports only what the APK's minSdk needs (v3, at 29), so the
  APK read as v3-only until verified with `--min-sdk-version 24`. No flag is needed.
- **Android starts `android_main` again in the same process.** After Back, `android_main` returned and the process
  stayed cached; the next launch was warm, the same pid, a new `android_main` on a new thread. Right after boot, a
  configuration change the manifest did not claim (input devices enumerating) destroyed the first activity before it
  had a window, and a second `android_main` ran. kama's `main` is a whole program, so it runs once per process: the
  manifest kama writes claims every configuration change, and when `main` returns the runtime finishes the activity,
  answers its last events and exits the process.
- **A panic's message could be lost** to the race between the logcat pipe's reader and `abort()`. bionic's
  `android_set_abort_message` puts it in the crash report (`Abort message:`), so `kama__fail` calls it on every Android
  build, `adb shell` runs included. Measured: an out-of-bounds index in an app shows in logcat both ways.
- The library has no `DT_SONAME`; NativeActivity loads it by path, and the linker said nothing at API 34.
- Every `--shared` compile was handed `-shared`, which clang warned was unused once per unit; it moved to the link.

**Step 1, built `0.9.873`.** `"kind": "app"` and the `app` block, as ruled, checked wherever kama.json is read: an app
without `app.id`, a version with a tag or a part past 999, a password in the keystore, `app` on an executable, or more
than `android.permissions` on a library is refused. On a target with no app format an app is an executable. For
Android, `kama build` links the program as `lib/<abi>/lib<name>.so` beside the APK, with the NDK's glue compiled in and
`-landroid -llog`, writes `AndroidManifest.xml` there (or checks the project's own), and runs aapt2, `jar`, `zipalign -P
16` and apksigner, debug-signed with Android Studio's key (made with `keytool` when missing) or, `--release`, with the
project's and `env:` passwords. The runtime's `android_main` (`include/kama_android_app.h`, emitted under
`-DKAMA_ANDROID_APP`) pipes stdout and stderr to logcat as `kama`, runs `main`, then finishes and exits. Every Android
link passes `-z max-page-size=16384`. `tools/check-android-app.sh` holds the rules everywhere and, where an NDK, SDK and
JDK are installed, reads the APK back with aapt2, zipalign and apksigner, `--release` and `vr` included; running the APK
needs a device, so that was by hand, on the emulator.

**Next, step 2: the lifecycle as events (decision 4) — the brief of 2026-11-08, ruled the same day: all six as
recommended.** Before
it, an app's `main` ran to completion without answering the activity, so a `main` that ran past a few seconds held
the activity's pause and input, and Android reported it not responding. `std::app::run(tick:, state:)` was the seam
already: a plain loop natively and emscripten's main loop on the web (`include/kama_app.h`).

*Measured* (emulator `kama34`, API 34: a probe app logging every command the NDK r30 glue delivers, its looper pumped):

| what the user did | commands, in order |
|---|---|
| launch | `START`, `RESUME`, `INPUT_CHANGED`, `INIT_WINDOW` (+60–100 ms; 320×640), `WINDOW_RESIZED`, `CONTENT_RECT_CHANGED`, `WINDOW_REDRAW_NEEDED`, `GAINED_FOCUS` |
| Home | `PAUSE`, `LOST_FOCUS`, `TERM_WINDOW` (+0.4 s), `STOP`, `SAVE_STATE` |
| back to the app | `START`, `RESUME`, `INIT_WINDOW`, `WINDOW_RESIZED`, `WINDOW_REDRAW_NEEDED`, `GAINED_FOCUS` |
| rotate (the manifest claims every configuration change) | `CONFIG_CHANGED`, `WINDOW_RESIZED` (640×320), `CONTENT_RECT_CHANGED`, `WINDOW_REDRAW_NEEDED` ×2: the same window, resized in place |
| notification shade down, then up | `LOST_FOCUS`; `GAINED_FOCUS`: no pause |
| `am send-trim-memory … RUNNING_CRITICAL` | nothing: NativeActivity forwards `onLowMemory` only |
| screen off; on | `PAUSE`, `TERM_WINDOW`, `STOP`, `SAVE_STATE`, `LOST_FOCUS`; then as "back to the app" |
| Back | `PAUSE`, `LOST_FOCUS`, `TERM_WINDOW`, `STOP`, `INPUT_CHANGED`, `DESTROY`; the process exits |

- **Android's UI thread waits on the program** (read in the glue's source, `native_app_glue.c`): `onStart`, `onResume`,
  `onPause` and `onStop` until the program's thread reads the command; `onSaveInstanceState` until it is handled; and a
  destroyed window until the handler has **returned** (the glue clears `window` after it). So a program hears of a lost
  window synchronously or not safely at all: a queue the next tick drains lets it draw into a window Android has
  already destroyed. And until the loop first pumps, `onStart` waits, so work in `main` before `run` holds the
  activity's start; step 1's mains were short enough not to show it.
- **The app is resumed before it has a window** (60–100 ms at launch), so "resumed" and "may create surfaces" are two
  facts. Rotation is neither: the window survives and changes size.
- **A safe `std::app` is expressible today** (probe, `0.9.875`): an `Owned<App>` over a contract, boxed, `release`d to
  the C loop as its state pointer, `Owned.adopt`ed back and dropped when the loop returns natively. No `unsafe` in the
  program; one dispatch per tick.
- **The web already suspends**: a hidden tab runs no `requestAnimationFrame`, so emscripten's loop stops ticking, and the
  program is never told (DOM `visibilitychange` is the event, `emscripten_set_visibilitychange_callback` its binding).

*What others do:*
- **winit 0.31** splits what 0.30 conflated: `can_create_surfaces`/`destroy_surfaces` follow Android's window, and
  `resumed`/`suspended` the activity; surfaces must be dropped before `destroy_surfaces` returns; and a platform with no
  surface lifecycle calls `can_create_surfaces` once at start, so portable code creates surfaces in one place.
- **Rust's `android-activity`** hands events to a callback inside `poll_events(timeout, …)`, so the app picks whether a
  poll waits; typed accessors give the window, the `JavaVM`, the activity, the asset manager and the input events.
- **SDL3** delivers lifecycle events to `SDL_AppEvent` as they happen and blocks its loop while the app is paused by
  default (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, "1"), which a program may turn off.
- **Khronos's `hello_xr`** waits in the looper only when `!Resumed && !IsSessionRunning()`: an OpenXR session keeps
  being polled while the activity is paused, until the session ends, then the loop sleeps 250 ms per pass.
- **gomobile** sends `lifecycle.Event` with stages (dead, alive, visible, focused) through one event channel.

*The decisions (each ruled as recommended):*
1. **An event is a call, made on the loop's thread as it happens**, between ticks: from the looper pump on Android,
   from the DOM's `visibilitychange` callback on the web. Never a queue the next tick drains, since Android's window
   destroy waits for the handler and a hidden tab runs no next tick.
2. **`std::app` takes the app as an owned contract**, replacing `run(tick:, state:)` and `TickFn`:
   ```kama fragment
   type contract App for value, resource {
       fn bool tick();                   // false ends the loop
       fn void lifecycle(Lifecycle event);
   }
   fn void run(Owned<App> app);         // the loop owns the app; on the web `run` never returns
   ```
   The safe surface (GOALS 3a), and it states what is already true: on the web `main`'s frame is gone once `run`
   starts, so the state had to be on the heap, and today each program must know that and cast a raw pointer back. The
   seven callers (the `app_loop`, `locked_held_trap` and four `net_` fixtures, the WebGPU example) migrate in the same
   commit. The alternative keeps the function pointers and adds an `EventFn`: still `unsafe` at every call.
3. **`Lifecycle { Resumed, Suspended, SurfaceReady, SurfaceLost, FocusGained, FocusLost, LowMemory }`**, named so it does
   not collide with `@kama/window`'s `Event`, which a program imports beside it:

   | | Android | web | desktop |
   |---|---|---|---|
   | `Resumed` | `onResume` | before the first tick; tab shown | before the first tick |
   | `Suspended` | `onPause` | tab hidden | never |
   | `SurfaceReady` | the window exists | once, after the first `Resumed` | once, after the first `Resumed` |
   | `SurfaceLost` | the window goes: drop every surface before returning | never | never |
   | `FocusGained`/`FocusLost` | the activity's focus (the shade, Quest's system overlay) | never: the window package's | never: the window package's |
   | `LowMemory` | `onLowMemory` | never | never |

   `Suspended` is the moment to stop audio and persist, since Android may end a stopped process without another event;
   `SAVE_STATE` is not an event, because kama runs `main` once per process and a program persists itself. Rotation is not
   an event: the surface learns its new size as it does on a desktop resize (`ANativeWindow_getWidth`, an out-of-date
   swapchain). A window package on Android wraps the activity's window and is handed these events by the program.
4. **While suspended the loop does not tick**: it waits in the looper, using no CPU, until `Resumed` or the activity's
   end, as SDL3 does by default and a hidden tab does already. A program that must keep ticking (an OpenXR session, as
   `hello_xr` keeps polling) says so while it needs it: `std::app::tickWhileSuspended(on: true)` when the session
   begins, `false` when it ends. The alternatives, a third contract member every app writes, or a `tick` that returns
   a flow enum, put the rare case on every program.
5. **A package reaches Android through `std::app::android`**, compiled for Android only: `window()` (the
   `ANativeWindow`, valid from `SurfaceReady` to `SurfaceLost`), `activity()` (the `ANativeActivity`, which holds the
   `JavaVM`, the activity object, the asset manager, the data paths and the SDK level that OpenXR, JNI and `assets` need)
   and `setInputHandler(handler:)` (one `fnptr` given each `AInputEvent`, returning whether it was handled, so Back can
   be kept). All `unsafe`, since they hand out what C owns. The glue's own struct is not handed out: `std::app` owns its
   `onAppCmd` and `userData`, and a package that set them would stop the loop hearing anything.
6. **The activity's end ends the loop.** On `DESTROY`, after `Suspended` and `SurfaceLost` have been heard, `run`
   returns without another tick, the code after it is the cleanup, and `main`'s return finishes the process as in step 1.
   A `main` that never calls `run` keeps step 1's behaviour. Documented, not decided: work before `run`, and each tick,
   must be short, since Android's start, pause and window destroy wait for the loop.

**Step 2, built `0.9.876`.** As ruled. `include/kama_app.h` holds the loop for each host: the plain loop and
emscripten's (now told of `visibilitychange`, so a hidden tab is `Suspended`), and in an app the glue's looper,
pumped without waiting while resumed (or kept awake) and waiting while suspended, with `onAppCmd` mapped to
`Lifecycle`. The entry (`include/kama_android_app.h`) keeps the glue's app, its thread, the input handler and the
awake flag as one object each, installs the glue's `onInputEvent` and clears it when `main` returns.
`std::app::android::setInputHandler` panics off the loop's thread, which is what its `@callerThread` type promises.
`quit` is no longer `unsafe`, like `run`. The seven callers moved to an `App`. `tests/app_loop` checks the order and
the drop on native and wasm; `tools/check-android-app.sh` builds an app on `std::app::android`. On the emulator
(2026-11-08, a probe drawing into `window()` each tick and counting input): launch, Home and back (`Suspended`,
`FocusLost`, `SurfaceLost`, no tick until `Resumed`), rotation (no event; ticks run), the shade (focus only), screen
off with `tickWhileSuspended(on: true)` (ticks ran while suspended), taps (both reached the handler, down and up,
once the window had drawn: Android routes no touch to a window that has never shown a frame), and Back (`run`
returned, the app was dropped, the process exited). A test script that launched before the package manager had
finished an `adb install` saw the new process killed under it; the runtime heard and answered that too.

**Step 3, its two small parts, built `0.9.877`.**
1. **The JDK Android Studio bundles.** A machine with Android Studio often has no `$JAVA_HOME`, so kama looks for its
   `jbr/` the way Flutter's `android_studio.dart` does (read 2026-11-08). On Windows and Linux each install records
   where it lives in the `.home` file of its `Google/AndroidStudio<version>` system directory; Linux's two usual unpack
   places count too. On macOS it is an `Android Studio*.app`, versioned by its `Info.plist`. The newest install wins.
   **The order, ruled 2026-11-08:** `$JAVA_HOME`, then Android Studio's, then `jar` on PATH. The plan had Studio's last;
   it moved ahead of PATH because macOS keeps a stub `jar` on PATH with no JDK installed, which would hide it. Flutter
   puts Studio's ahead of `$JAVA_HOME` as well, and kama does not, since an explicit setting wins. apksigner finds its
   `java` only through `$JAVA_HOME` or PATH, so a JDK from Studio is handed to it as `$JAVA_HOME`. With no JDK at all,
   the build used to fail late, as `jar` or `keytool` exiting non-zero; it is now refused up front, naming all three
   places. `check-android-app` installs a fake Android Studio for each host's layout and proves both that it is used,
   with apksigner on it, and that it comes before PATH.
2. **`kama seed --kind app`.** It writes the `app` block and a `main` that runs an `App` on `std::app`, so a new app
   starts on the loop rather than as a program that runs and finishes. Its `tick` greets once and returns false, which
   keeps `kama run` finite here and is the first program part 3 runs on a device. `id` is asked for on a terminal, or
   taken from `--id`, and without one the seed is refused, since it has no default. `targetSdk` is the newest platform
   the SDK has, else 34.

**Next, the rest of step 3 — the plan for the session that builds it (written 2026-11-08).** `kama run` on a device
(part 3) shipped `0.9.878`, its brief and record below. Left:
4. **`assets`** (M–L, brief first): a directory the manifest names, packed into the APK's `assets/`, and a std reader.
   The question is the shape. An Android-only reader over `AAssetManager` (`std::app::android`), or one way to read a
   file the program ships on every host: Android's asset manager, a directory beside the executable natively, the
   bundle on iOS, and on the web a preloaded file system or a fetch. Go's `golang.org/x/mobile/asset.Open` is the
   cross-platform shape, and SDL reads Android assets through its ordinary file calls; read both, and what Godot and
   Bevy do, before recommending. Writing the APK's `assets/` uses `aapt2 link -A`, to be proven by hand first.

**Step 3's `kama run`: the brief of 2026-11-08.** Today `kama run` of an Android app is refused, naming `adb install`.
What it should do instead is what a native run does: build, start the program, show what it prints, and end with its
status, on a device.

*Measured* (emulator `kama34`, API 34, abilist `x86_64,arm64-v8a`; a probe app printing for 10 s and returning 7, and
one that panics on an index past the end):
- **Install.** The build writes v4's `.idsig` beside the APK, so a plain `adb install -r` tries an incremental install
  first. On the emulator that failed and fell back to a streamed one: 835 ms. `--no-incremental` streams directly: 559
  ms. Where incremental succeeds, the APK is still arriving when the app starts, which is the race step 2's probe hit
  (the first launch killed). adb's source (`adb_install.cpp`) confirms the default: incremental, falling back on
  failure.
- **Launch.** `am start -W -n <id>/android.app.NativeActivity` waited for the launch and reported `Status: ok`,
  `LaunchState: COLD`, about 0.6 s. With the app already running, the intent went to the running copy: same pid, no
  new `main` ("Activity not started, intent has been delivered to currently running top-most instance"). `am start -S`
  stopped it first and started a fresh process.
- **Output and the end.** One `adb shell` running the following streamed the program's lines, as it printed them, with
  no prefix, from `main starts` to `main returns 7`, and ended 0.3 s after the process did:
  `logcat -v raw -T 1000 --pid=<pid> -s kama & L=$!; while [ -d /proc/<pid> ]; do sleep 0.2; done; sleep 0.3; kill $L`
  `pidof` is on the device.
- **A panic.** A panic's message reached the `kama` tag: `kama: panic: DynamicArray index out of bounds
  (crashprobe/main.kama:9)`.
- **The exit status.** Android records how the process ended, in `ApplicationExitInfo` (API 30 and later), which
  `dumpsys activity exit-info <id>` prints. A return from `main` reads `reason=1 (EXIT_SELF) status=7`, the panic
  `reason=5 (APP CRASH(NATIVE)) status=6` (SIGABRT), and a force-stop `reason=10 (USER REQUESTED) subreason=21 (FORCE
  STOP)`. API 29 has no such record.
- **Killing the client.** Killing the local `adb shell` mid-run, which is what Ctrl-C does to it, left the app running,
  and the logcat on the device too. Only `am force-stop` ended the app.

*What others do* (read from source 2026-11-08):
- **cargo-apk:**
  - picks a device with `-d/--device <serial>`;
  - follows the device's ABI only when no target is given, and falls back to arm64 without a word;
  - installs with `install -r`;
  - shows `logcat --uid`, which never ends on its own;
  - stops nothing on Ctrl-C.
- **xbuild:**
  - force-stops the app, then `push` and `pm install`;
  - shows `logcat -T <start time> --uid`;
  - reports no exit.
- **Flutter:**
  - picks the one device, prompts on a terminal when there are several, and refuses without one;
  - builds for the device's ABI;
  - installs with `install -t -r`, and on any failure uninstalls and retries, which deletes the app's data;
  - scans `am start`'s output for errors, since its exit code is 0 even on failure;
  - stops the app on Ctrl-C and on `q`, and leaves it running on `d` (detach).
- **None of the three** detects the app's end or reports its status, and none reads `$ANDROID_SERIAL` itself. adb does:
  one device is the default, `$ANDROID_SERIAL` picks one, `-s` overrides it, and with several and neither, adb refuses
  ("more than one device/emulator").

*The decisions — ruled 2026-11-08: 2, 6 and 7 as recommended, 1 revised to add `--device` (the maintainer asked
whether a flag was the more production-grade answer; it is, as below), and 3, 4, 5 and 8, which the measurements
decided, as written:*
1. **The device is adb's to choose, and `--device <serial>` overrides it.** The flag is passed as adb's `-s`. Without
   it, adb's own rule applies: one attached device is used, `$ANDROID_SERIAL` picks one of several, and with several and
   neither the run is refused, listing the serials `adb devices` gives. The recommendation was the variable alone. The
   flag won because it applies to one command and shows in it, and works the same in every shell: PowerShell and cmd
   have no `VAR=x cmd` form, so there the variable would hold for the whole session. It is also adb's own layering (`-s`
   over `$ANDROID_SERIAL` over the one device), the one cargo-apk's and Flutter's `-d` follow.
2. **The target is the triple the command names, checked against the device.** `kama run kama.json` with no `--target`
   runs the app here, where it is an executable. On a device, `--target aarch64-linux-android29` names the ABI and the
   minimum level, and the run is refused when the device's `ro.product.cpu.abilist` lacks that ABI or its SDK level is
   below the triple's, naming the triple that would run. An x86_64 emulator lists `arm64-v8a` too, through its ARM
   translator, so the triple a Quest needs also runs there, more slowly. Following the device's ABI on its own, as the
   test leg does, would make the program built depend on which device is plugged in.
3. **Install:** `adb install -r --no-incremental`, from the APK `kama build` writes into `out/`, which is kept as a
   build's output. A device refusing it is reported, never repaired: an app signed with another key
   (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`) or a lower version names `adb uninstall <id>`, since uninstalling deletes the
   app's data, which Flutter's silent retry does.
4. **Launch:** `am start -S -W -n <id>/android.app.NativeActivity`. `-S` because `main` runs once per process, so a
   copy already running would be handed the intent and run no new `main`. `-W` and its `Status:` line say whether it
   started.
5. **Output:** the process's own lines, from its start until it ends, through the one `adb shell` measured above, on
   `kama run`'s stdout as a native run prints them, the panic line included. Arguments after `--` are refused for a
   device run: the launcher gives `main` no arguments.
6. **The status is Android's record.** When the process is gone, `kama run` reads its `ApplicationExitInfo` (by pid)
   and exits with it as a native run would: `EXIT_SELF` with its status, a crash with 128 plus the signal, which is 134
   for a panic. Anything else (killed, force-stopped) is named and exits 1. It is the platform's own account, it covers
   every way a process ends, and it needs no change to the runtime. On API 29, which keeps no record, the run says so
   and exits 0 once the output has ended. The alternative is a line the runtime logs as `main` returns: it works at API
   29, but it is a second channel kama would own, and it says nothing about a crash outside a panic.
7. **Ctrl-C stops the app** (`am force-stop`), then exits 130, as a native program dies with its run and as `flutter
   run` stops its app. Leaving it running would leave the device's logcat running too (measured), and the app keeps no
   terminal to return to. A program meant to outlive the run is installed with `kama build` and adb, as today.
8. **adb is the SDK's** `platform-tools/adb`, from the same SDK the build uses (`$ANDROID_HOME`, else Android Studio's
   default), else `adb` on PATH. The build and the run then agree on one SDK.

**Built `0.9.878`, as ruled.** The device is checked before anything builds, and the build goes to `out/` as `kama
build`'s does. The process's lines are read a line at a time, dropping logcat's `--------- beginning of` banner. An app
that has already ended by the time `pidof` asks (the seeded one greets and returns at once) has its lines dumped by
the pid its exit record names. Two things the build found:
- On Ctrl-C the device's shell went with the local adb, but the logcat it had put in the background did not, and kept
  following a pid that no longer existed. It is now stopped with the app.
- `runCmdCapture` had not taken `runCmd`'s outer pair of quotes on Windows, which keeps cmd.exe from stripping a
  command that starts with a quoted program path. Every adb call starts with one, so it now does.

*Proven on the emulator* (`kama34`, 2026-11-08):
- the seeded app prints `hello from seeded` and exits 0;
- the probe streams all 25 lines and exits 7;
- the panic prints its line and `ended on signal 6 (APP CRASH(NATIVE))` and exits 134;
- a second run of the same app starts fresh;
- each refusal reads as written above: arguments, an unknown `--device`, `--device` on a run here, an ABI the device
  lacks, and a level above the device's;
- Ctrl-Break, sent to `kama run`'s process group from Python, which is the event Ctrl-C's handler shares, stops the
  app, exits 130, and leaves no logcat behind. msys2's `kill -INT` is not that test: it terminates `kama.exe` outright.

POSIX's Ctrl-C (SIGINT to the foreground group: kama's handler, adb's default) is reasoned, not run, since no device is
attached to a POSIX box here. `check-android-app` holds the refusals that need no device, since starting adb in a guard
leaves its server running on every CI runner.

Also waiting, not this step's: the engine's amendments to `@kama/gpu` and `@kama/window` (§8), written into
`s8-gpu-wrapper` and `s8-input` once they settle and are ruled; kama's red-flag read of them went to the engine
2026-11-08.

Open for the headset step: whether `vr` also writes the OpenXR loader's entries (the
`org.khronos.openxr.permission.OPENXR` permission and a `<queries>` for the runtime broker, which Khronos's Android
loader needs from Android 11, and Meta's own loader does not), and the network security configuration Meta's page asks
for without naming the attribute.

**Decided 2026-11-02** (the maintainer took the production answer for each; 3 replaces a first step proposed the day
before, and 4 is a direction whose shape KR-283 designs; the rulings on the target itself shipped, and docs/targets.md
records them):
1. **Output goes to logcat through the app glue**, when the launcher starts the app; `adb shell` runs keep their
   stdout. Go's gomobile does the same.
2. **The emulator first, the headset last.** The headset gates only the last step, the VR launch on Horizon OS.
3. **`kama build` makes the APK** for an Android app, as it makes the HTML harness for wasm: the `.so` with the NDK's
   `native_app_glue` compiled in, a manifest the project supplies or `kama.json` generates (with room for Meta's
   entries), then aapt2, zipalign and apksigner from the SDK's newest build-tools, debug-signed with a generated key
   by default and release-signed with the project's keystore. The SDK's tools are external, as clang and emcc are.
   The commands are proven by hand first, never shipped as a recipe. `kama.json`'s shape is the first step.
4. **`main` stays the entry on every target** (direction; the shape is designed here). The runtime supplies
   `android_main`, runs kama's `main` on the glue's thread, and `std::app::run` pumps the looper and calls the tick,
   as emscripten's loop does on wasm; the lifecycle (pause, resume, a lost window) reaches the program as events, and
   a package (a window, XR) reaches the activity through a std accessor. A program that wants the glue to itself
   still writes its own `@linkName("android_main")` entry.

**Checked 2026-11-05: what Android expects, and that kama runs its tools rather than writing its own.** The maintainer
asked that kama never rewrite a platform tool the platform requires.
- **The tools are Google's.** `aapt2` is the resource compiler Android Studio and the Gradle plugin run, documented for
  direct use (`aapt2 link -o app.apk -I android.jar --manifest AndroidManifest.xml`). `zipalign` and `apksigner` are the
  SDK's, in that order: a v2+ signature covers the whole file, so aligning after signing breaks it. `d8` is needed only for
  Java or Kotlin code, and a `NativeActivity` app with `android:hasCode="false"` has none. Gradle with the Android Gradle
  Plugin is the build system that drives these same tools for a Java project, which kama does not have; Unity, Unreal,
  Flutter and Tauri generate one because their apps carry Java. So decision 3 runs the platform's tools. What would be a
  rewrite is gomobile's binary XML written without `aapt2`, and xbuild's own packaging, now unmaintained; both are refused.
- **16 KB pages.** Since 2025-11-01 Google Play requires an app with native code that targets Android 15 or later to support
  16 KB pages: every `LOAD` segment of a `.so` aligned to 16 KB, and an uncompressed `.so` zip-aligned on a 16 KB boundary
  (`zipalign -P 16 4`, checked with `zipalign -c -P 16 -v 4`) ([developer.android.com](https://developer.android.com/guide/practices/page-sizes)).
  *Measured here:* NDK r30 links kama's `aarch64-linux-android29` output with every `LOAD` at `2**14` by default. An older
  NDK does not, and kama passes nothing for it, so KR-283 passes `-Wl,-z,max-page-size=16384` on every Android link and the
  hand proof uses `-P 16`. Quest is not Play, but a 16 KB device refuses a misaligned library wherever it came from.
- **The stores.** Meta's takes an APK, with OBB and asset files beside it, through `ovr-platform-util upload-quest-build`
  ([Meta](https://developers.meta.com/horizon/documentation/native/ps-assets/)). Google Play has required an Android App Bundle
  for a new app since August 2021, which Google's own `bundletool build-bundle` makes from the same compiled parts without
  Gradle. So an AAB is a later step, for phones, and through `bundletool`, never a writer of kama's.

**The toolchain on the Windows box** (2026-11-02, under `%LOCALAPPDATA%\Android`, every download hash-checked): the
NDK; the SDK's `cmdline-tools`, `platform-tools` (adb), `build-tools;37.0.0` (aapt2, zipalign, apksigner),
`platforms;android-34`, the emulator with `system-images;android-34;google_apis;x86_64` (AVD `kama34`, headless under
WHPX in about 35 s); and a portable Temurin JDK 21 for `sdkmanager` and `apksigner`, which a script names in
`JAVA_HOME`. For the last step, the headset: developer mode (a Meta developer organisation, then the Meta Horizon app),
a USB-C cable, and its USB-debugging prompt accepted, after which `adb devices` lists it.

<a id="s5-ios"></a>

### An iOS target (KR-281)

Filed 2026-11-01 from the same engine's platform plan, which lists iOS. Probed on `0.9.803` by transpiling only,
since building needs a Mac (Apple licenses its SDK only on Apple hardware). **The iOS work is done on the Mac.**

- **Where kama is.** `--target IOS` is refused. `aarch64-ios-none` derives `OS_IOS`, so std's `OS_MACOS` declarations
  (`std::net`'s `ListenOptions`, for one) fall to their generic branch. A triple's components are its only flags, so
  nothing names the Darwin half that macOS and iOS share. Xcode's spelling, `arm64-apple-ios`, is refused, naming
  `aarch64-ios-none` (`0.9.808`).
- **What it needs beyond the triple.** Apple's clang and the iOS SDK. An app whose loop the OS owns: it runs
  `UIApplicationMain` and drives frames from `CADisplayLink`, the inverted loop wasm already has. And a `.app` signed
  with a provisioning profile. `dlopen` is barred, so KR-24's hot reload stays desktop-only.
- **The verdict: scheduled, not yet designed.** It is a target of a general-purpose systems language, so it is not
  optional. Android's principle carries over, kama runs the platform's own tools, and `main` stays the entry. Also to
  decide: how std spells the Darwin family.
- **Corrected 2026-11-05: on iOS the platform's tool is Xcode itself, so kama drives `xcodebuild` and never builds
  around it.** This page said `kama build` would make the signed `.app` as it makes an APK. App Store Connect validates
  the build environment Xcode writes into `Info.plist` (`DTXcode`, `DTXcodeBuild`, `DTSDKName`, `DTSDKBuild`): a beta
  Xcode's values fail the upload (ITMS-90111, ITMS-90534), and since 2026-04-28 an upload must be built with the iOS 26
  SDK or later. Every other language that ships iOS apps goes through Xcode: gomobile runs `xcrun xcodebuild` on a project
  it generates; Tauri's cargo-mobile2 generates one with XcodeGen and builds it with `xcodebuild`; a Flutter app is an
  Xcode workspace (`ios/Runner.xcworkspace`), where signing is set; Unity and Godot export an Xcode project. **The
  expected shape, to rule on the Mac:** kama builds the program as a static library for `aarch64-ios-none`, generates an
  Xcode project around it from `kama.json`'s `app` block (§ Android, decision 2, one id for both), and `kama build` runs
  `xcodebuild`, which compiles the asset catalog and launch screen, signs with the provisioning profile and exports the
  `.ipa` (`-exportArchive`). kama writes no `Info.plist` build keys, signs nothing and assembles no `.app` itself.

<a id="s5-stack-bound"></a>

### A computed stack bound per entry point (KR-336)

SPEC § *The stack* states each target's stack: 8 MiB for a 64-bit native `main`, 2 MiB on 32-bit, 256 KiB on wasm. The
one check a project can make is the C compiler's per frame (`-Wframe-larger-than=N -Werror=frame-larger-than` in its
`cflags`), and a frame guard does not bound a chain. **Measured `0.9.860`:** ten functions, each holding an
`InlineArray<int32>#(7000)` (28,000 bytes, under a 32 KiB guard) and calling the next, build clean on both targets;
natively the program prints `sum 45`, and under node the wasm build fails with "Stack overflow detected. You can try
increasing -sSTACK_SIZE (currently set to 262144)". Nothing at build time said so. On Windows the guard cannot be set
below about 132 KB at all once `std::fs` is imported, since kama's own path frames are 64 KiB by design
(`docs/platforms/windows.md`). A web game is where this bites: the browser's stack is the small one, and what overruns
it is a chain of systems, engine modules and kama's own calls, on a path someone happens to take.

**The ask, from a game engine on kama and seconded by another project as wanted but not blocking:** the worst-case
stack depth of each entry point (`main`, each isolate, each `parallel_for` worker, and each pool job once KR-3 builds
them), computed from the call graph with kama's runtime frames counted, reported against the stack the target states
with the chain that exceeds it, as `@noheap` reports a chain. A recursion leaves the bound unknown, which is KR-79's
*no recursion*; this is that property with a number on it. It needs each function's frame size, which only the C
compiler knows: clang's `-fstack-usage` writes it per function (`.su` files), and the call graph kama builds already
holds the edges. Indirect calls (a `fnptr`, a contract slot) bound only by their targets' maximum, as `--no-heap` treats
a contract slot. Unscoped: whether it is a property a program declares, a build report, or a target's default, wants
KR-79's design pass first.

**What others do.** GCC's `-fstack-usage` writes each function's frame, and `-fcallgraph-info=su` (GCC 10) writes the call
graph beside it. Clang and emcc have `-fstack-usage`. A tool after the build sums the deepest path: GNATstack for Ada and C,
`cargo-call-stack` for Rust (on nightly's `-Z emit-stack-sizes`). No mainstream compiler refuses a program whose bound
exceeds the stack it was given.

**Measured on the Mac, 2026-11-07:** both C compilers kama drives write the sizes, host clang and emcc 6.0.13 for wasm, into
`<output>.su`. A `--release` wasm build of the same chain fails too, as `RuntimeError: memory access out of bounds`. At
`-O2` a callee the C compiler inlines is counted in its caller's frame and again in its own, so a sum over kama's graph
overcounts it: safe, but pessimistic. Two more questions for the design pass: how an inlined frame is counted (GCC's
call-graph file says, and clang has none), and that a bound belongs to a build, not to the source, since the sizes change
with target and optimization level.

<a id="s5-math-float64"></a>

### `float64` vectors, quaternion and matrices (KR-343)

`std::math`'s linear algebra is `float32` only (`vec.kama` opens "Float32 vectors"), while the scalar surface takes both
widths through `Real<T>` (`scalar.kama`: "One name per operation, at both float widths"). A game engine on kama asked, and
does not need them itself: its positions are a zone and a `float32` offset, so every system stays in `float32`. Its owner's
question was whether this is a kama gap at all. **It is, for a general-purpose language:** a double-precision physics
backend, a CAD or GIS tool, astronomy, or any world kept in absolute coordinates needs `float64` vectors, and every maths
library compared has both widths: glam (`Vec3`/`DVec3`), Unity.Mathematics (`float3`/`double3`), GLM (`vec3`/`dvec3`), DirectXMath
and Eigen (`Vector3f`/`Vector3d`). Not blocking anyone today.

**The design question:** separate types (`DVec3`, `DQuat`, `DMat4`), which is what the others do and costs a second copy of
every method, or one generic `Vec3<T: Real<T> = float32>`, which kama can say (a contract bound and a default type parameter)
and which keeps `Vec3` meaning what it means now: one name per operation at both widths, as the scalars already have it.
Measure first whether the generic instance compiles to the same code as today's `float32` type (`Real<T>`'s adapters are
inlined calls), and what it does to error messages and `kama query`'s signatures. Verdict: scheduled, design first.

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

- **General shared memory ("hybrid") stays out; one shape of it shipped (`0.9.730`–`0.9.733`).** Co-equal
  shared-memory threading is still not planned, because it reintroduces the hazard the model removes. What shipped is
  `Locked<T>`/`ReadWriteLocked<T>`: a lock that owns its value, reachable only inside a `borrow` window that can neither
  wait (`@parks`) nor take another lock — SPEC *Shared mutable state* — and `ConcurrentMap`, a map sharded over many
  (`0.9.736`, SPEC *A concurrent map*). The other seams stay as they are: `Atomic<T>`, immutable `Shared` and disjoint
  `parallel_for`.


<a id="s6-fibers"></a>

### Fibers and thread-per-core: blocking-shaped I/O for many connections (KR-3)

**The design is [docs/design/fibers.md](design/fibers.md)** (2026-10-22): what the tree has, what nine other runtimes
do, fifteen decisions each with a recommendation, the registration measurement, what this brief had wrong, and an
eleven-commit implementation map. In short: one isolate per core, each a cooperative fiber scheduler over its own
`Poller`; a fiber never leaves its isolate, so it needs no `Sendable` bound, no colouring and no `Pin`, and freedom
from data races keeps holding by construction.

**Ruled 2026-10-22: all fifteen as recommended**, each to be verified as it lands. By number, each argued in the design:
1. Thread-per-core with fibers, in std, as part of the isolate: the first `spawn fiber` makes an isolate a scheduler,
   and its `scope` runs it.
2. The switch: own asm per architecture on POSIX (measured on arm64 at about 21 ns a round trip, 25–35 times cheaper
   than `swapcontext`), the OS Fiber API on Windows, an unmodelled arch refused. Stacks are a 256 KiB reservation with
   a guard page, stated by `spawn fiber(stack:)`, with an overflow diagnostic.
3. **No park inside a lasting access to `Shared` storage**, a compile-time walk on `--no-heap`'s engine (`@parks`
   is the leaf). This replaces "exclusivity per fiber", which was inverted, and it leaves the switch nothing to save —
   amended 2026-10-25, building it (`0.9.743`): the leaf is a real fiber switch read off the call graph, since
   `@parks` now also marks waits that switch nothing (`println`), and the roots are every isolate entry, `main`
   included, whose reach includes a fiber spawn, since fiber 0 parks too (fibers.md decision 3, *As built*).
4. wasm: fibers over JSPI, which every engine now ships unflagged, as the second phase.
5. Two schedulers: per-isolate fibers for I/O, and a sized job pool for CPU work whose jobs may not park.
6. Registration, measured on epoll and kqueue (`bench/poller/`): readability edge-triggered once on both, and every other
   change batched into the next wait, so a park costs no system call. One-shot costs 8–13% on epoll, and an unbatched
   kqueue re-arm 6%. AFD stays one-shot. It is internal: the public `Poller` stays level-triggered.
7. Transparent: the same `TcpStream.read` parks on a fiber (Loom, Go, Eio), not Zig's explicit `Io` parameter.
8. Channels become fiber-aware, joins park, and regular files and DNS go to an offload pool (with KR-202); a pipe parks as a socket does (built `0.9.786`).
9. `spawn fiber f(b: …)`, the isolate's fn-plus-one-bundle. A `Fiber<R>` handle whose join yields `R` — amended
   2026-10-25: the join CONSUMES the handle, `R r = give f.join();` (Rust's `JoinHandle::join(self)`), so a
   second join is a compile error. A panic still aborts the process.
10. A fiber `spawn` may sit anywhere in its `scope`, which keeps a runtime list of live children (an accept loop).
11. A timer heap per scheduler (high-resolution on Windows, as Go did in 2024), per-operation deadlines, and
    cooperative cancellation as `Err(Cancelled)` — amended 2026-10-26: a socket deadline is Go's, an absolute
    `Instant` kept on the socket (`setReadDeadline`/`setWriteDeadline`/`setAcceptDeadline(at: Optional<Instant>)`, a
    `ConnectOptions` deadline through `connectWith`), persisting until reset and failing a late call even with data
    queued; one meaning on a thread (a one-socket poll for the time left). One `sleep`: `std::time::sleep` parks.
12. Cooperative. I/O is polled every N runs (Go uses 61), and there is a `yield()`.
13. Descriptor lifetime: DEL before close, deferred record frees, and deregistration on `give` — corrected
    2026-10-26: nothing runs at a hand-off (`Sendable` is a marker; a bundle is a byte copy), so the isolate that next
    uses or drops a socket registered elsewhere DELs it from that queue itself and hands the record back to its home
    scheduler, which frees it on its next turn. A duplicated descriptor shares the mode, so the POSIX fiber path passes
    `MSG_DONTWAIT`.
14. Sanitizer fiber annotations and an lldb OS plugin, in the first commits.
15. MCU and `@noheap`: parking allocates nothing; no park inside `@onPanic`. Fibers on bare metal are KR-233 —
    corrected 2026-10-26: a socket's first wait registers it, which allocates, and the `@noheap` walk is program-wide,
    so a program that runs fibers anywhere refuses `@noheap` socket I/O and `sleep` everywhere (KR-245), and since
    step 6 channel waits and joins, which reach the scheduler's timer heap and its loop.

Steps 1–6 shipped `0.9.737`–`0.9.751` (fibers.md §8 records each, and the as-built notes under decisions 1, 3, 7, 8, 9,
10 and 13). Step 5 was ruled 2026-10-26 and built `0.9.745`–`0.9.748` (fibers.md decisions 7, 11, 13 and 15): `Poller.wait`
parks too, on its own descriptor on epoll and kqueue and through step 7's offload pool on AFD and `poll(2)`; two fibers
on one socket came as `split` in `0.9.845`. Step 6 (`0.9.751`) made channels and isolate joins park, with no new surface.

**Step 7, where it starts (read 2026-10-27; re-probe before building).** Decision 8's third part: work that blocks and
has no readiness to wait on runs on a small per-process pool of threads while the fiber parks, and the pool's completion
wakes it through `kama_sched_wake`, the remote queue channels use since step 6. Nothing of a pool exists yet:
`parallel_for` spawns its isolates per call.
- **Regular files.** `File.read` and `write` are `kama_read`/`kama_write` (`lib/std/fs/fs.kama` ~475, ~487; `kama_os.h`
  ~2039 POSIX, ~399 Windows), and `read` of a regular file never reports `WouldBlock` on any OS. Which calls go to the
  pool is a decision: the data calls, and the metadata calls that can stall on a network filesystem (`open`, `stat`,
  a directory read).
- **DNS (with KR-202).** `std::net::resolve` (`lib/std/net/addr.kama` ~318) is `kama_resolve_host` (`kama_os.h` ~4209,
  `getaddrinfo` ~4226), which blocks for seconds and takes no timeout. On a fiber it parks while the pool resolves, and
  a deadline lets the fiber leave while the late answer is dropped, which is KR-202's shape.
- **`Poller.wait` on AFD and `poll(2)`.** `kama_poller_wait_parked` (`kama_os.h` ~4452) parks on the Poller's own
  descriptor where there is one (epoll, kqueue) and blocks the thread where `kama_poller_fd` is -1.
- **`offload(…)`.** A foreign C call that blocks stalls its core; decision 8 names an explicit
  `std::concurrent::offload(…)` that runs it on the pool, as tokio's `spawn_blocking` does. Its spelling is language
  surface and wants a ruling: a function taking an entry and one bundle, as `spawn` does, or a statement form.
- **Rulings owed before code:** the pool's size and growth (libuv's is 4 threads, `UV_THREADPOOL_SIZE`; tokio's grows to
  512 blocking threads), the `offload` spelling, which file calls go to the pool, and whether a thread with no
  scheduler keeps calling inline (the recommendation: yes, no pool hop where nothing else could run).

**Ruled 2026-11-04: KR-3 in full is next on the Mac, unhurried, over as many sessions as it takes.**
The aim is threading in a good place for its consumers, so no step is narrowed or deferred to finish sooner. A downstream
engine has asked for jobs, which is why it moved up, and that request does not outrank the design.
- **Scope:** steps 7 to 11, and what those decisions left unbuilt: KR-296 (decision 11), KR-297 (decision 12), KR-202 (inside
  step 7), KR-229 (inside step 8), and closing KR-255 on step 9's rerun. The other rows of the fibers arc stay queued.
- **Who:** step 8 and step 7's `Poller.wait` over AFD belong to the Windows box, since this Mac could only test them through
  CI. The Windows box also holds defects and blockers meanwhile.
- **Order:** one brief for both pools' spelling first, then step 9, then 7, 10, KR-297, KR-296 and 11. The order is not critical
  to correctness: step 9 relies only on steps 4 and 6, both shipped. Writing the one brief is what keeps it from costing the
  design anything. **What it costs:** building 9 first is untried, and the process-wide thread start and stop written for it
  gets adapted for 7.
- **Where the job pool lives:** kama code in `std::concurrent` (`lib/std/concurrent/`), not a package, since its no-park rule is
  in the compiler. `spawn job` beside `spawn` and `spawn fiber`, or a library call, is the brief's question: KR-255's record
  only sketches the first.

**Written 2026-11-04: [the pools' brief](#s6-pools), rulings owed.** **That brief must also answer,** from a game engine on kama that runs the subsystems of a frame phase in parallel, with the
compiler proving their access (reported 2026-11-04, each reproduced on `0.9.844`):
1. **Handing out work must be cheap enough for a barrier per phase.** `parallel_for` starts and joins its threads on every call.
   Measured on `0.9.844` (release, M4 Mac, 1,024 elements, the minimum of 5 runs of 1,000 calls): 15 µs at 1 worker, 23 at 2, 33 at
   4, 70 at 10, 100 at 16. The reporter measured far more on Windows (`0.9.793`): 0.15 ms at 1, 1.34–1.38 at 16, 1.83 at 20. A dozen
   barriers a frame at 16 workers is 7% of a 60 Hz frame on the Mac, and all of it on Windows.
2. **The pool must run borrowing work.** Decision 5's unit is "a `Sendable` job", moved in, and a moved job cannot borrow a frame's
   stores, so the proof the compiler gives `scope` and `parallel_for` would be lost where the performance is, leaving `UnsafePtr`.
   The ask: a `scope`'s children and a `parallel_for`'s workers run on the pool's threads, as Rayon's `scope` does. **This reopens
   decision 5** ("`parallel_for` stays the data-parallel layer"), and may leave `spawn job` with little to do: the pool could be
   what the existing forms run on. It also has to answer what a child loses by not being an isolate on its own thread: its stack
   (`spawn(stack:)`), per-thread state, and how a panic ends it.
3. **No heap allocation per hand-out.** `parallel_for` under `@noheap` is refused ("heaps a per-worker argument bundle"), and the
   reporter wants allocation up front, with `@noheap` as the proof.

And children sharing a read-only borrow (shipped `0.9.858`), a `spawn` over several places (shipped `0.9.859`) and KR-315 (parts chosen at run time)
are the same design: what a child may borrow from a phase's data, and what proves it. **Not settled here:** when the reporter first
needs the pool, which it leaves to a measurement, so it does not set the order on its own. How the job-entry rule identifies a job is
this brief's as well, if `spawn job` survives.

<a id="s6-pools"></a>

### The job pool and the offload pool: one brief (KR-3)

**Status: brief written 2026-11-04 on `0.9.845`, revised 2026-11-05 on `0.9.850`, measured on Windows 2026-11-05 on `0.9.852`. Ruled 2026-11-05: all 17 decisions as recommended below, each option named here refused.
The game engine that asked audited it the same day, on `0.9.852`: nothing in it conflicts with a design the engine has decided, its answers to KR-326 and KR-327 are in those rows, and its needs are added to KR-320, KR-325, KR-298 and KR-79.** It answers what `s6-fibers` left to
it, for
step 9 (the job pool) and step 7 (the offload pool) together, with readers sharing a `const ref`, a `spawn` over several places and KR-315. Everything marked *measured* comes from
`bench/jobs/` and can be rerun, the compiler's half from `bench/jobs/probes/` (the readers' scratch patch landed in `0.9.858`); results are in
`docs/benchmarks/jobs-*.tsv`. Everything else says *predicted*.

**Revised 2026-11-05, after the maintainer asked whether this still fits a general-purpose language.** It did not, in three places,
all shaped by the frame loop that prompted the brief: decision 5 refused a `parallel_for` that reads a file, decision 4 made a process
multi-threaded before `main`, and decision 7 took away per-worker state with nothing in its place. Decisions 4, 5 and 7 change,
decisions 2, 8, 14 and 15 are corrected, and decisions 16 and 17 are new. `cpuCount()`'s limits (Linux `0.9.852`, Windows `0.9.855`) and KR-325 came out of the same probes, and so did a data race `parallel_for` let through, fixed in `0.9.851`.

**What changes from what was ruled or said, plainly.**
- **"Jobs must not park" (decision 5, ruled 2026-10) goes in part.** A job may wait on the OS, and may not wait on another task. That
  ruling was for a separate job pool; moving `parallel_for` onto the pool would have carried it onto every data-parallel loop.
- **Decision 5 is reopened and two of its sentences go.** "`parallel_for` stays the data-parallel layer" goes: it runs on the
  pool. "Its unit
  is a `Sendable` job" goes: the unit is a record on the caller's stack, and it may borrow.
- **Decision 5's "a channel the job completes" contradicts its own rule.** `Sender.send` reaches a fiber switch (measured below),
  so the no-park
  rule refuses it. A fiber waits at a `scope`'s brace instead, which needs no channel and no heap.
- **What the maintainer was told is wrong in one part.** "A library in `std::concurrent`, with `spawn job` as a possible language
  form" becomes: the pool
  is kama code in `std::concurrent`, and `spawn job` is a language form it cannot do without (decision 2).
- **KR-255's `Job<uint32>` sketch is replaced** by the `scope` above; the held `Job<R>` remains for work that outlives a block
  (decision 3).

**The answer, in one example.** A frame phase: tasks that differ, then an even split. Every borrow is proven at compile time; no
line of it allocates.

```kama fragment
fn void step(ref World w, float32 dt) {
    scope {                                                  // the brace is the barrier
        spawn job integrate(p: ref w.pos, v: ref w.vel, dt: dt);   // writes pos, reads vel (`const ref` in its signature)
        spawn job steer(a: ref w.ai, v: ref w.vel);                // reads vel too: two readers share it
        spawn job mix(e: ref w.emitters);
    }
    parallel_for (ref Chunk c in w.chunks, chunks: cpuCount() * 4) { c.settle(); }
}
```

The bytes of `parallel_for`, today and proposed (the first is what `0.9.845` emits; the second is predicted):

```c
// today: per worker, a heap bundle, a heap control block and a thread
kama_isolate_t __pfh0[__pfk0];
for (int __pfw0 = 0; __pfw0 < __pfk0; ++__pfw0) {
    kama_kama_pf_arg0* __pfa0 = kama_alloc(sizeof(kama_kama_pf_arg0), _Alignof(kama_kama_pf_arg0));
    __pfa0->slice = View__slice(&__pfv0, __pff0, __pfc0, "win.kama", 8);
    __pfh0[__pfs0++] = kama_isolate_spawn(&kama_kama_pf_tramp0, __pfa0);
}
for (int __pfj0 = 0; __pfj0 < __pfs0; ++__pfj0) kama_isolate_join(__pfh0[__pfj0]);

// proposed: one record on this frame, and no allocation
kama_kama_pf_ctx0 __pfx0 = { __pfv0, __pfch0 };
std__concurrent__poolRun(&kama_kama_pf_chunk0, &__pfx0, __pfk0);
```

#### What was measured

**The prototype is `bench/jobs/pool.c`,** a prototype of the hand-out path, not the runtime: records on the forker's stack; a
hand-out from a thread outside
the pool is one group on a list, its chunks claimed by a `fetch_add`; a fork on a worker goes on that worker's fixed Chase-Lev
deque. TSan-clean in every
combination of spin, help and shape. A checksum over the work has to agree across every arm in a round: 0 disagreements in 1,372
runs (M4).

**A phase with real work: microseconds per barrier, once every 1.39 ms (12 in a 60 Hz frame), workers idle between.** M4 (4P+6E),
7 rounds interleaved, median, p99 in
brackets, the pool sized to the machine at 10 threads. The serial cost is 59 µs in the first table and 1,004 µs in the second.

| chunks | `parallel_for` today | pool, 50 µs spin | pool, 2 ms spin | Rayon, caller on a worker | Rayon, caller outside | Rayon `scope` |
|---|---|---|---|---|---|---|
| 2 | 62 (75) | **47** (50) | 68 (243) | 51 (55) | 60 (85) | 54 (62) |
| 4 | 73 (102) | **38** (55) | 41 (128) | 78 (155) | 108 (166) | 104 (150) |
| 10 | 102 (125) | **39** (56) | 22 (104) | 108 (166) | 109 (170) | 98 (157) |
| 16 | 134 (159) | **36** (60) | 20 (66) | 105 (171) | 102 (169) | 88 (163) |
| 40 | 307 (362) | **38** (60) | 18 (97) | 101 (171) | 102 (166) | 81 (162) |

| chunks | `parallel_for` today | pool, 50 µs spin | pool, 2 ms spin | Rayon, caller on a worker | Rayon, caller outside | Rayon `scope` |
|---|---|---|---|---|---|---|
| 2 | 547 | **524** | 772 | 529 | 535 | 529 |
| 4 | 328 | **304** | 448 | 352 | 383 | 379 |
| 10 | 281 | **260** | 224 | 295 | 291 | 270 |
| 16 | 279 | **235** | 231 | 266 | 283 | 256 |
| 40 | 361 | **209** | 203 | 247 | 255 | 232 |

CPU burned beyond the one core the caller spins on, at 10 chunks and the shorter phase: today 0.2 cores, pool 0.4, **2 ms spin
8.6**, Rayon 0.5-0.7.

**What it does to a frame.** Twelve of the shorter phases at 10 chunks: today 1.22 ms (7.3% of 16.7 ms), pool 0.47 ms (2.8%),
Rayon 1.30 ms. An ideal split of the same work is 0.07 ms.

**Four things this says.**
1. **Long spinning is a loss, and that is what lets a pool be simple.** A 2 ms spin gets the best median at 10 chunks and over on
   short phases, 18-22 µs against 36-39, by burning 8.6 of 10 cores
   continuously. It loses outright at low chunk counts (772 against 524 at 2), and its p99 reaches 1,687 µs. libgomp's default
   spin is 300,000 iterations for the same reason, and a GCC
   report (PR 43706) records it slowing work that shares its cores. The recommendation is a 50 µs spin and no knob (decision 6).
2. **A static split is set by the slowest thread.** At 1 ms and 10 chunks on 4P+6E nothing is near the ideal 100 µs, and 40 chunks
   on the same threads take 209 µs against 260, which says a chunk on a slower core
   holds the barrier (inferred from that, not measured per core). So the count a hand-out states is a real decision, and more
   chunks than threads must be cheap, which they are not today (361 µs at 40, thread per chunk). See decision 13.
3. **A hand-out stops paying for itself below about 40 µs of work on this machine.** The 59 µs phase settles at 38 µs however many
   chunks it has, where an even split would reach 6, so about 30 µs goes to waking sleeping workers
   (inferred). That is the OS's wake, and only spinning avoids it. A phase below it should run serially, which `chunks: 1` states
   at no cost (0.3 µs).
4. **Rayon is ahead in one shape, and it is stated.** Back-to-back barriers with no work and the caller already on a worker: 0.1
   µs at 2 chunks and 2.8 at 10, against the pool's 2.1 and 3.9 from outside
   it, with workers spinning throughout. From outside the pool, where a frame loop lives, Rayon takes 12.5 and 14.8 µs. On
   recursive forks the pool ties: fib(36) forking down to fib(10) takes 4.10 ms on 9 workers
   against Rayon's 4.03 on 10, and 4.26 on 9. A bare central list lost this by 100 times (441 ms) on the first attempt, which is
   why workers have deques.

**Linux arm64, with OpenMP and Go** (a VM of 6 vCPUs, not nested, 5 rounds, median, 820 runs, 0 checksum disagreements; `bench/run
jobs`). Serial 57 µs, then 1,009 µs:

| chunks | `parallel_for` today | pool, 50 µs spin | pool, 2 ms spin | Rayon, caller on a worker | Rayon, caller outside | Rayon `scope` | OpenMP (gcc) | Go |
|---|---|---|---|---|---|---|---|---|
| 2 | 87 | **59** | 52 | 84 | 117 | 87 | 58 | 75 |
| 4 | 98 | **54** | 32 | 90 | 135 | 97 | 45 | 67 |
| 6 | 142 | **55** | 26 | 92 | 142 | 57 | 97 | 60 |

| chunks | `parallel_for` today | pool, 50 µs spin | pool, 2 ms spin | Rayon, caller on a worker | Rayon, caller outside | Rayon `scope` | OpenMP (gcc) | Go |
|---|---|---|---|---|---|---|---|---|
| 2 | 580 | **557** | 672 | 583 | 624 | 589 | 538 | 652 |
| 4 | 367 | **359** | 380 | 387 | 424 | 395 | 312 | 413 |
| 6 | 388 | **368** | 280 | 396 | 412 | 346 | 359 | 424 |

**Where the pool is behind here, and it is stated:** OpenMP at 4 chunks, by 20% and 15%, and at 2 chunks by 3%; Rayon's `scope` at
6 chunks and 1 ms, by 6%. I do not know why OpenMP leads at 4 and not at 6 (97 against 55): a team the size of the hand-out wakes
nobody it does not need, which is the first thing to look at when this is built. Go is close on short phases. On recursive forks
the pool leads, 3.56 ms against Rayon's 5.38, and a short phase barely gains from sleeping workers in this VM (54 µs against 57
serial), where a wake crosses a hypervisor. Bare metal is unmeasured.

**wasm under node** (`jobs-wasm.tsv`, 5 rounds, main on a pthread as a browser build runs it): a phase of about 60 µs serial takes
322 µs today at 4 chunks and 58 on a pool; with no work, 243 and 2.4. So a pool removes the cost and gains no speed on a phase
that short, the same floor as above.

**Windows, where the reporter's numbers are worst** (`jobs-windows.tsv`: Windows 11 on an AMD Ryzen AI 9 465, 10 cores and 20
threads, Balanced power plan; `0.9.852`; msys2 UCRT64's gcc 16.2, Rust 1.99 and Go 1.27; 7 rounds, median, p99 in brackets, 1,988
runs, 0 checksum disagreements; the pool sized to the machine at 20 threads, parked on `WaitOnAddress`). Serial 74 µs, then 917 µs:

| chunks | `parallel_for` today | pool, 50 µs spin | pool, 2 ms spin | Rayon, caller on a worker | Rayon, caller outside | Rayon `scope` | OpenMP (gcc) | Go |
|---|---|---|---|---|---|---|---|---|
| 2 | 262 (459) | **91** (194) | 98 (2,051) | 93 (202) | 98 (207) | 84 (190) | 102 (242) | 94 (239) |
| 4 | 389 (583) | **66** (126) | 145 (2,054) | 75 (142) | 93 (155) | 77 (150) | 97 (183) | 85 (234) |
| 10 | 713 (1,022) | **59** (129) | 144 (2,045) | 108 (201) | 124 (237) | 107 (201) | 96 (184) | 78 (190) |
| 16 | 1,047 (1,405) | **59** (128) | 138 (2,038) | 107 (238) | 119 (245) | 117 (247) | 115 (204) | 75 (184) |
| 20 | 1,252 (1,704) | **59** (118) | 163 (2,048) | 103 (232) | 118 (258) | 115 (245) | 129 (218) | 74 (203) |
| 40 | 2,405 (3,083) | **58** (122) | 152 (2,038) | 112 (273) | 125 (288) | 127 (340) | 206 (342) | 78 (213) |

| chunks | `parallel_for` today | pool, 50 µs spin | pool, 2 ms spin | Rayon, caller on a worker | Rayon, caller outside | Rayon `scope` | OpenMP (gcc) | Go |
|---|---|---|---|---|---|---|---|---|
| 2 | 739 | **638** | 756 | 598 | 587 | 592 | 638 | 623 |
| 4 | 632 | **351** | 435 | 382 | 385 | 389 | 428 | 459 |
| 10 | 766 | **199** | 295 | 242 | 259 | 240 | 223 | 251 |
| 16 | 1,088 | **167** | 348 | 212 | 219 | 208 | 205 | 235 |
| 20 | 1,330 | **151** | 227 | 201 | 211 | 202 | 201 | 225 |
| 40 | 2,491 | **136** | 280 | 199 | 207 | 191 | 251 | 208 |

**What it says.**
1. **The reporter's number is the thread start, and the pool removes it.** `parallel_for` today costs about 65 µs a chunk here with
   no work (650 µs at 10 chunks, 1,071 at 16, 1,275 at 20, 2,499 at 40), 9.4 to 10.7 times the M4's, which is what was predicted;
   the reporter's 1.34-1.38 ms at 16 and 1.83 at 20 on `0.9.793` are the same slope on another machine. Twelve of the shorter phases
   at 10 chunks take 8.55 ms today, **51% of a 60 Hz frame** (7.3% on the M4); on the pool 0.71 ms (4.3%), Rayon 1.29, OpenMP 1.15,
   Go 0.94. An ideal split is 0.09.
2. **The pool with a 50 µs spin leads every phase from 4 chunks up, and stays flat as chunks pass threads:** 58-59 µs on the shorter
   phase from 10 to 40 chunks, where Rayon takes 103-127, OpenMP 96-206 and Go 74-78; 136-199 µs on the 1 ms phase against 191-259
   for the others. On the shorter phase its p99 is the lowest in every row from 4 chunks: 118-129 µs, where Rayon's is 142-155 at
   4 chunks and 201-340 from 10.
3. **A wake costs about 50 µs here, against about 30 on the M4** (inferred as there): the shorter phase settles at 58-59 µs from 74
   serial, where an even split would reach 4-7. So a hand-out pays for itself above about 60 µs of work on this machine, and
   `chunks: 1` below it.
4. **A 2 ms spin loses outright,** to the 50 µs spin in every row of both tables (138-163 µs against 59 on the shorter phase from 10
   chunks), and to Rayon in most of them, its p99 at 2.0-2.6 ms
   throughout, burning 17.5 of 20 hardware threads. On the M4 it won the median from 10 chunks; here every spinning worker shares a
   core with another hardware thread, the caller's among them, so spinning slows the work it waits for (inferred, not measured per
   core). Decision 6's 50 µs and no knob hold more strongly on this machine than on the one that chose them.
5. **Where the pool is behind, and it is stated:** at 2 chunks, Rayon's `scope` by 8% on the shorter phase (84 against 91) and
   Rayon by 7-9% on the 1 ms phase (587-598 against 638), with Go 2% ahead there; and back-to-back barriers with no work, where
   Rayon with the caller on a worker takes 1.1 µs at 2 chunks and 5.0 at 10, and Go 1.9 and 3.6, against the pool's 5.4 and 8.9
   from outside it (Rayon from outside, 15.1 and 16.4). **Recursive forks:** the pool leads, fib(36) forking down to fib(10) in
   2.46 ms on 20 threads against Rayon's 3.59.
6. **What each burns** beyond the caller's core, at 10 chunks on the shorter phase: today 0.9 (its thread starts), pool
   0.3, 2 ms spin 17.5, Rayon 0.0-0.5, OpenMP 0.0, Go 0.1. The CPU is the child's `GetProcessTimes`, as `run.py` reads it on Windows.

`kama#2` against `kama`, paired by round: median 0.998, every pair within 0.79-1.28 but two, both in round 2, of unknown cause (a
monitor of other processes' CPU covered rounds 3-7 and found none over a quarter core): one run of each took three times its usual
(1,192 µs against 385, 1,647 against 720), and no median moves. Go's arm reads QueryPerformanceCounter on Windows, as Rust's `Instant` and the C arms' `clock_gettime` do: Go's own monotonic
clock there advances with the timer interrupt, and read a barrier as 0 µs or a whole tick.

#### What a child loses by not being an isolate on its own thread

| | a `spawn` child today | a job on the pool | evidence |
|---|---|---|---|
| its stack | stated, 8 MiB by default, `stack:` raises it | **nothing lost**: 8 MiB by default, `stack:` runs it on a pooled stack through the switch KR-3 already owns | measured: 100,000 frames over 50 MiB on a 64 MiB stack, 26-50 ns a job; overflow reports through the guard page |
| module `static`s | a fresh copy at the initialiser | **refused**: a job cannot reach one (decision 7) | measured: `main=7 first=100 second=100` |
| a panic | aborts the process | **nothing lost**: aborts the process | read: `kama_panic` |
| `@onPanic` | per thread | **nothing lost**, and a hand-out stays refused inside one (decision 9) | measured: refused today only because it allocates |
| the `Shared` access record and the lock record | per thread | **nothing lost**: a chunk's writes to its elements consult neither, and it cannot name another thread's `Shared` | read from the emitted C and the `Sendable` gate; re-probe with ASan when built |
| waiting on another task | allowed, and hangs if that task is queued behind it | **refused** where the graph can see it (decision 5) | measured, per call, below |
| waiting on the OS | allowed | **allowed**, and it holds one worker until it returns (decision 5) | measured: `probes/parfor_files.kama` compiles and runs today |
| running fibers | allowed | **refused**: no scheduler on a pool thread | by the same walk |

#### The audit against GOALS.md

| decision | #1 library in kama | #3 alloc | #3a-c safety, kinds | #4 one way | #5 explicit | fits practice? |
|---|---|---|---|---|---|---|
| 1 what runs on the pool | yes | none per hand-out | unchanged | **one** data-parallel form | `spawn job` greppable | **no on one point:** a source break on what `parallel_for` may do |
| 2 a language form | the pool yes, the proof no | — | the proof stays safe | a third `spawn` word beside `fiber`, and decision 15 is a fourth | yes | yes, as `spawn fiber` |
| 3 held `Job<R>` | yes | heap, visible, refused under `@noheap` | `Sendable` moved | same shape as `Fiber<R>` | yes | yes |
| 4 one process pool | yes | at its first hand-out; up front when `@noheap` code hands out | — | one | **partly:** started implicitly, at a hand-out the program wrote | yes for hosted programs, as Rayon, Java and OpenMP; before `main` under `@noheap` |
| 5 no waiting on a task | — | — | proof of no hang, not of no stall | — | names the chain | yes, the walk kama already has; the OS wait is every pool's answer |
| 6 what a barrier does | yes | none | — | one per caller kind | — | yes |
| 7 statics refused | — | — | stronger than the isolate's | — | names the chain | **no:** other pools give per-worker state; decision 16 answers the need instead |
| 8 `stack:` kept | yes | a stack per stated size, cached | guard page | one clause | yes | yes |
| 9 panic | — | — | a new refusal, to stay sound | — | — | yes |
| 10 shared readers | — | — | same proof `parallel_for` rests on | no new spelling | **partly:** the signature says `const`, as a call's does | yes |
| 11 several places | — | a stack record | each place judged alone | removes a rule | yes | **no:** rewrites "the bundle is the boundary" |
| 12 chosen parts | — | none | checked at run time, as `xs[i]` | no new API | the index is visible | yes |
| 13 `chunks:` | — | — | — | — | the name says what it is | a rename, 26 fixture files, no peer |
| 14 pool in kama | **yes** | — | a bounded `unsafe` seam | — | — | **no:** every sibling seam is C, and measured no slower; no private OS symbol |
| 15 offload pool | yes | a stack record, or held | — | same family, and its line against `spawn` is the thinnest here | greppable | yes |
| 16 bind a chunk | — | none | a view, as the element's is | one more binding on one form | the chunk is in the source | yes: `par_chunks`, OpenMP's split |
| 17 a view to a job | yes: parallel algorithms in std | none | prefix rule on the host | — | yes | yes: `split_at_mut` |

No goal is traded against the best measured answer anywhere here. Where the pool is behind another runtime is stated above.

#### The decisions

**Decision 1 — what runs on the pool.**

| | pool form | a thread form beside it | a pool job may block? |
|---|---|---|---|
| Rust | Rayon `scope`, `par_iter` | `std::thread::scope` | yes, and it stalls a worker |
| C++ | TBB, OpenMP | `std::thread` | nothing checks |
| Java | parallel streams on the common pool | `Thread`, virtual threads | through `ManagedBlocker`, which may add a spare thread |
| C# | `Parallel.For` on the thread pool | `Thread` | the pool adds threads when starved, and Microsoft's guidance is not to block |
| Unity | `IJobFor` | managed `Thread` | nothing checks |
| Go | one scheduler for everything | none | yes: a blocked goroutine costs little |

**Every language with a pool but Go keeps a thread form beside it. Why:** a thread may wait for another, and a sized pool's job waiting
on a queued sibling hangs. Go can do without because its scheduler adds threads around a blocked goroutine. So `spawn` stays `std::thread::scope` and `parallel_spawn`
stays a thread per element, and neither moves.

**Recommended: `parallel_for` moves to the pool, and `spawn job` is new.** `parallel_for` already forbids what a pool cannot do:
"an element may not wait on another element's progress" (SPEC), and `parallel_spawn` is already where to go
if it must. Moving it silently speeds every existing use.

- **Option A (recommended). Sacrifice:** a source break, in four parts, and the count below does not measure them, because no peer has
  a `parallel_for` in code at all: the corpus cannot say what a general program does here.
  1. A body may no longer wait on another task (decision 5). SPEC already forbids it, unchecked, and it hangs; a bounded `send` to a
     consumer on its own isolate is legal today and does not hang, and it is refused too.
  2. `workers: 64` no longer runs 64 blocking calls at once: `chunks: 64` runs as many as the pool has threads. **This one compiles
     and goes slower,** so the rename in decision 13 is what makes a reader look at it. Many waits at once are fibers, or
     `parallel_spawn` over workers draining a channel.
  3. A body may not reach a module `static` (decision 7).
  4. A body may not call through a plain `fnptr` (decision 5).

  In-tree, after the rename: 0 of 13 fixtures (14 sites read one by one; `fiber_isolate_join` sleeps in one, which now passes;
  `isolate_stack` states `stack:`, which decision 8 keeps; none touches a module `static`).
- **Option B: `parallel_for` stays and a pooled twin arrives. Sacrifice:** two data-parallel forms, and the one that looks obvious
  is 2.6 times slower on a real phase and 33 times on a trivial one, at 10 chunks. GOALS #4.
- **Option C: `spawn` moves too, whenever the walk proves it waits on nothing. Sacrifice:** a debug `send` silently moves a task
  off the pool, a performance cliff with no text at the site. GOALS #5.

Consistent with practice on everything but the break, and pre-1.0 allows it.

**Decision 2 — a language form, not a library call.** A `ref` cannot be stored (GOALS 3e), so a safe library cannot hold a borrow in a
queue. `std::thread::scope` and `rayon::scope` rest on lifetimes, which kama rules out on purpose. **Closures are not the reason, and
this paragraph first said they were:** a closure in kama would capture by move or by `Shared` (§2's verdict, 2026-09-07) and add no
borrowed capture, so adding them would leave this decision where it is. What the lack of them costs here is a named function per
task, and a functor type for a library's own algorithm: `probes/parfor_functor.kama`, a generic `parMap<F: Mapper>` carrying state,
runs today with a direct call. TBB began in that shape. The proof lives in `scope`, so the form has to as well. A library spelling means `UnsafePtr`, and the compiler's proof
stops there, which is what the reporter asked to avoid. **Recommended: `spawn job`, contextual as `fiber` is (the lexer's
lookahead),
so `job` stays usable as a name.** A direct statement of its `scope`, as an isolate's is, so the count is read off the block and
the records sit on the frame; a count known only at run time is `parallel_for`. **Sacrifice:** a third `spawn` word, and a job in
a loop is refused where a fiber's is allowed. Four
artifacts, as every grammar change.

**Decision 3 — the held form.** `Job<R> j = spawn job decode(a: give asset); … R r = give j.join();`, the shape of `Fiber<R>`, for
work that outlives a block: decode started in one frame and collected three later, which is Unity's `JobHandle`. A moved
`Sendable` bundle, on the heap, refused under `@noheap` where it
is seen. A join runs the job on the spot if no worker has taken it. **Verdict: scheduled,** after the scope form. **Sacrifice:**
one more type; a long held job occupies a worker that a barrier may want, which is the priority question below.

**Decision 4 — one process pool, started at its first hand-out.** *Revised 2026-11-05: this first recommended starting it before `main`
in any program holding a form.*

| | pool | when its threads start | who sizes it |
|---|---|---|---|
| Rayon | global, plus any you build | first use | `build_global`, `RAYON_NUM_THREADS` |
| Java | `commonPool`, plus any you build | first use | a system property |
| OpenMP | one, per team | first region | `OMP_NUM_THREADS` |
| Unity | one | engine start | `JobsUtility.JobWorkerCount`, lowered while running, up to a maximum |
| Go | one scheduler | **in every program, before `main`:** *measured,* 5 threads at `main` with `GOMAXPROCS` 6 (Go 1.27.3, Linux arm64, 6 vCPUs), and more on demand | `GOMAXPROCS`, settable while running |

- **A (recommended): at the first hand-out, or at `setJobThreads(count: n)` if the program calls it first.** `cpuCount()` threads counting
  the caller; `count: 1` makes every form run on its caller, in order, so one binary can run serial and parallel. **A program whose
  `@noheap` code reaches a hand-out starts the pool before `main`,** since that code cannot, and the walk knows it from what the
  program says: `@noheap` already means "allocate before here". **What it gives:**
  - A program that never reaches a form at run time pays nothing, and a program sizing the pool first starts nothing twice.
  - A process stays single-threaded until it does otherwise. *Measured* (`probes/before_main/`): `unshare(CLONE_NEWUSER)` succeeds
    single-threaded and answers `EINVAL` with one other thread, which is why a Go container runtime does that work in a C constructor
    (runc's `nsexec`); a thread started before `fork` is gone in the child (5 threads, then 1); and a sandbox applied per thread
    (seccomp without `TSYNC`) would miss threads already running.
  - **wasm pays far more than native, and only if it uses the pool.** *Measured under node:* the first thread takes 13 ms, and ten more
    take 8-15 ms on the caller and 66-77 ms until all run (4 runs), against about 15 µs a thread on the M4. Before `main`, every page holding
    a form anywhere would pay that at load. Browsers are unmeasured.

  **Sacrifice:** a thread start the program did not spell, at a hand-out it did: about 150 µs at 10 threads here, and about ten
  times that on Windows, where a thread's start and join measured 9.4-10.7 times the M4's (`jobs-windows.tsv`), and 8 MiB reserved per thread with a few pages touched. One load and a branch
  on every hand-out (*predicted* under a nanosecond). A program that wants the cost elsewhere calls `setJobThreads` at its start.
- **B: before `main` in any program holding a form (the first recommendation). What it gives:** no cold first hand-out, and one rule, not
  two. **Sacrifice:** everything A lists, in every run of every program whose code holds a form, including a package's rarely taken path.
  Go is the precedent, and runc's constructor is what that precedent costs.
- **C: a pool handle passed to each form. Sacrifice:** a parameter through every signature, the shape decision 7 refused from Zig,
  and a library cannot parallelize inside without asking for one.
- **Rejected: a hand-out under `@noheap` on an unstarted pool runs on its caller.** No heap, but a loop silently serial because a start
  was forgotten, which is the cliff option C of decision 1 was refused for.

**The bytes** (*predicted*): the hand-out above, behind `if (!started) poolStart();`, and with no branch at all when the program starts
the pool before `main`. Both arms keep the walk honest: the start is reached only where it may allocate.

**To build with it, none of it in the first version of this brief:** workers start with every asynchronous signal blocked, so a signal a
program expects on its own thread does not land in a worker, and `SIGSEGV` and `SIGBUS` stay open for the overflow handler. `pthread_atfork`
returns the child's pool to unstarted, as `kama_fiber.h` already registers one. Priorities (a long held job competing with a barrier)
are **not in this design**: KR-320 holds that verdict, since a pool has to exist before a priority can be measured. **The pool's size rests on
`cpuCount()`, which counts what the process may use, on Linux since `0.9.852` and on Windows since `0.9.855`.**

**Decision 5 — a job may not wait on another task; it may wait on the OS.** *Revised 2026-11-05: this first refused anything that reached
`kama_fiber_switch`.* That refused the plainest parallel program there is, and the probe says so: `probes/parfor_files.kama`, a
`parallel_for` calling `readFile` per path, compiles and runs today, and `readFile` reaches a switch (`probes/reach_readfile.kama.refused`
names the chain). It would have had no replacement before step 7, and kama alone in the table in decision 1. One rule was covering
two different harms:

| a job waits on | what it costs the pool | can it hang the pool? |
|---|---|---|
| another task: a channel, a join | every worker can end up waiting on work queued behind it | **yes** |
| the OS: a file, `sleep`, a socket | one worker, until the call returns | no: the call returns without the pool's help |

**Recommended: the walk's leaves are the task waits,** `kama__channel_park`, `kama__isolate_park`, `kama_isolate_join`, `kama_sched_scopeWait`
and a fiber's join, on the lock walk's `kRuntimeWaits`. A job entry is identified as a thread entry is, by its trampoline's prefix
(`__kama_job_`, with `kama_kama_pf_tramp`). A contract call is judged against every implementer. A nested pool form is no wait, since the
waiter takes work.

| a job may | refused |
|---|---|
| `println`, `logInfo`, allocation, `monotonicNow`, `Atomic`, a `Locked` window, `tryRecv`, **`File.read`, `readFile`, `sleep`, `TcpStream`, `Poller.wait`** | `Sender.send`, `recv`, `recvTimeout`, an isolate's join, `spawn fiber` |

- **Why any rule, when Rayon has none:** projects report deadlocks from a job waiting on what a waiting worker's frame holds, and Java and C#
  add threads to cover it. kama has a whole-program walk, and a refusal naming the chain beats a hang. It also turns SPEC's unchecked "an
  element may not wait on another element's progress" into a compile error, where today it hangs in silence.
- **KR-298 `trySend` moves into scope,** as a job's only send. A `Locked` collection is the other way out.
- **A blind call (`fnptr`, a bound function) is refused,** as both existing walks refuse it, with `@noparks fnptr` on the named type as the
  way through. On the model of `@noheap fnptr`, the marker is part of the type and checked where a function is bound to it: a proof,
  not a promise. **Sacrifice:** a table of plain function pointers needs the marker; contracts and generics need nothing.
- **Sacrifice of the rule:** what October's ruling gave. A job that blocks holds a worker in silence: a frame can stall on a slow read, and a
  fiber parked at a brace waits longer, since it takes no chunks. The width is the pool's, so a hundred slow requests are fibers, not
  `chunks: 100`. **A program that wants the proof asks for it per function, with KR-79's *no blocking syscall*,** which is that row's own
  example and now has a use; until KR-79 ships, nothing offers it. **Stated residuals,** as `Locked` states its own: a job blocked on
  a socket or pipe whose other end is another job, or a fiber at a brace, hangs; so does a foreign C call that waits on one; a spin on an `Atomic`.
- **Option: refuse every park (the first recommendation). Sacrifice:** no file, no `sleep` and no socket in any `parallel_for`, a
  rule no other language has, for a harm that is a stall and not a hang. It is the right proof for a frame loop and the wrong default for
  a language.
- **Not in this design: a spare thread when a job blocks,** as Go, Java and C# have. Rayon has none. Reopened on a measurement.

**Decision 6 — what a barrier does, by who is waiting.**

| caller | does | why |
|---|---|---|
| a fiber | parks, and takes no chunks | a chunk would stall every other fiber on its core, on a 256 KiB stack; the last chunk wakes it through `kama_sched_wake` (*predicted*: one park and one wake on top) |
| a thread outside the pool | takes its own chunks, then waits | helping others risks a long stranger under a short join. Its own chunks alone rule out a deadlock: nothing waits on a chunk nobody took |
| a worker (a nested form) | takes any work | or it idles with work about, as Rayon's does. *Measured:* parity above |

A bounded 50 µs spin before sleeping, with no knob: a knob was rejected above on the numbers, and a caller that wants workers hot
can call a phase more often. **A verdict on the knob: a non-goal,** reopened on a measurement where 20 µs a barrier matters more
than 8.6 cores.
**Rayon's real deadlock cannot happen here:** a waiting worker picks up a job that needs a lock its own frame holds (reported
against Rayon repeatedly), and kama already refuses a join inside a lock window at compile time (`checkLockWindows`). The pool's
wait joins that list.
**A job never runs on a fiber** (a fiber never helps, a job cannot start one). The walk needs no cut for it now that its leaves are task
waits. **Sacrifice:** nesting piles frames on a worker's stack, as Rayon's does.

**Decision 7 — module `static`s: a job cannot reach one, and decision 16 replaces what that took.** A chunk may run on the caller, so it
would see *main's* copy and could change it, on one chunk out of k; on a worker it would see whatever the last job left. *Measured
today:* a `spawn` child starts at the initialiser, and a `parallel_for` worker has its own copy for all its elements
(`probes/parfor_scratch.kama` prints `1 4 1 4 main=0`), **which is what per-worker scratch is today, and the first version of this decision
took it away with "pass it in, one per chunk", though `parallel_for` has no way to pass one.** Unity's own documentation says jobs get no
protection here and that it intends to prevent it by static analysis. **Recommended: refused, read or write, by the `@foreignEntry` walk,
naming the chain,** so a job's result cannot depend on which worker ran it. In std that touches one static, `uuid.kama:58` (so a job cannot
make a v7 UUID until that is carried in). **Sacrifice:** a library that keeps a `static` cannot be called from a job. **Option: per-worker
statics, as every other pool has (`thread_local!`, `threadprivate`). Sacrifice:** a result that depends on scheduling, which is the bug a
world hash exists to catch, and one chunk on main's copy unless the caller's chunks move too.

**Decision 8 — `stack:` is kept.** `spawn job(stack: n) f(…)` and `parallel_for (…, stack: n)` run on a stack of that size on a
pool thread, through `kama_fiber_switch`, with its guard page and handler. **Measured** (`kama_fiber.h` from C, on a 64 MiB
stack): 100,000 frames deep, 26 ns a job with a kept stack, 50 ns freed to the idle list, ASan-clean. Rayon has only a pool-wide
`stack_size`, so kama is alone here, **because** it owns a switch and a Rust
library does not (the `stacker` crate is the nearest). The overflow message names the job's clause. **Sacrifice:** a mapping per
stated size, on Windows through the Fiber API (*unmeasured there*), and **it inherits KR-238:** where shadow stacks are enforced, on Linux
x86-64, the switch refuses with a message, so a stated stack that works on a thread today would refuse until that row ships. **Option: refuse `stack:`, send deep recursion to `spawn`.
Sacrifice:** `isolate_stack` and the clause go, for nothing measured.

**Decision 9 — a panic, and `@onPanic`.** A panic aborts the process on any thread, unchanged. **A new refusal:** a hand-out
inside an `@onPanic` region stays refused. Today that holds only because `parallel_for` allocates (*measured*); once it does not,
a `longjmp` past the barrier would leave workers holding pointers into a dead frame. It goes on the region's walk, with an xfail.
**Sacrifice:** none anyone had.

**Decision 10 — readers share (shipped `0.9.858`).**

| | what shares a read | what makes a type safe to |
|---|---|---|
| Rust | `&T` across `thread::scope`, Rayon | `T: Sync`, a second trait, since `Cell` is `Send` but not `Sync` |
| Swift | any `Sendable` | one protocol, with `@unchecked` for a class that locks |
| Unity | `[ReadOnly]` on a container | the safety system, at run time |
| C++ | anything | nothing checks |

**Kama would have one contract where Rust has two. Why that is right here:** Rust needs `Sync` because safe code can mutate
through `&` (`Cell`, `RefCell`). In kama `const` is deep: *measured*, a write through `const this` is refused, through an `Owned`,
through a collection, and through a non-`const` method. The only safe interior mutability is `Atomic` and `Locked`, which are safe
to share. And **`parallel_for` already rests on this:** *measured,* eight workers
reading one captured `Sendable` resource through a `const fn` compile today and are TSan-clean. Readers sharing adds no new exposure; it
removes an inconsistency.

*Measured with a scratch compiler (the conflict test in `isolatePrep` reads the parameter's constness and a reader passes another
child's freeze, about a dozen lines):* two readers of one place, and a reader of a place beside a reader of its field, are
accepted; eight readers each making 200 passes over a `DynamicArray`, a `Map` (`get` and `valuesConst`) and a `string` are
TSan-clean; and these stay refused: writer beside reader in both orders, a whole-place reader beside a writer of its field, a
write in the scope body, and a type that is not `Sendable`. A scan of `lib/std` found no `const fn` that writes through `unsafe`.
**Recommended: `Sendable` alone, no new spelling, and the message fixed** (it calls a read a write). `c: const ref c` stays
unparsed: the signature says it, as a call's does. **Stated rule:** an `unsafe` body writing under a `const fn` of a `Sendable`
type must make that write safe to race; `parallel_for` already asks that, unstated. **Sacrifice:** `ref` at the call site does not
show that the child only reads.
Size S. Consistent with practice. **Shipped `0.9.858`, one step wider than the patch:** a reader child passes every freeze
whose holder only reads (another reader, a window over a `ConstView`, a `foreach` that writes nothing, a `match` arm reading
in place), and one whose holder may write refuses it by name, where it had said "growing or reseating".

**Decision 11 — several places (shipped `0.9.859`).** `spawn integrate(p: ref this.pos, v: ref this.vel, dt: dt);`. "One parameter because the
bundle *is* the boundary" protects nothing for a borrow: `parallel_for` already passes every capture as its own `ref`, in a
generated record. **Recommended: an entry takes any number of parameters, for every `spawn` form:** each `ref` held to the prefix
rule as its own child, each `give` to its own `Sendable` gate, and a value copied in, which today needs a `const ref`.
**Sacrifice:** SPEC's rule is rewritten, and a generated record replaces the one pointer, on the frame for a scope child and on
the heap for a held one. **Option: borrows only, a moved bundle stays single. Sacrifice:** a job cannot take a moved buffer and a
borrowed table together, for nothing the rule protects. *Predicted* to cost nothing at run time: a record the same size as the
arguments. **As built, one correction:** "on the frame for a scope child" holds for an isolate child only, whose record sits beside
its handle until the join, one allocation fewer than the moved bundle's box. A fiber child's record is boxed, as a moved fiber
bundle always was: the spawn only queues the fiber, and in a loop the next pass would overwrite a frame record before it ran
(`fiber_several_args` holds that down). One `ref` parameter still passes the place's address, with no record. Building it found
a compiler crash, fixed with it: `spawn w();` against an entry with no parameters read a parameter that was not there, in both
forms, on every version to `0.9.858`; it is now legal.

**Decision 12 — KR-315, parts chosen at run time.** **This differs from the plan's lean,** a checked "pick" type: it needed a new
view kind and a sorted index list, and a smaller answer does more.

- **A fixed set of tasks:** `spawn step(col: ref cols[i]); spawn step(col: ref cols[j]);` is accepted over a contiguous std
  container, the indices checked distinct when the scope runs, trapping as `xs[i]` traps; constant equal indices refused at
  compile time. The body may not touch `cols` until the brace, as for any child. Rust's `get_disjoint_mut` answers a `Result`;
  kama's index already traps, so this reads as an index does.
- **A count known only at run time:** `parallel_for (ref Column c in cols, chunks: n) { if (!c.wanted(region)) { continue; } … }`,
  which exists today, and which cheap chunks make balance. Rayon's `par_iter_mut().filter()` is the same shape.

**Sacrifice:** a trap, not a compile error, when two indices are equal; a program that cannot rule that out checks first. A user
`operator[]` is excluded, since it could map two indices to one element. **Seen, no ask yet:** `parallel_for` binds an element,
not an index, so a system over two stores needs both in one element: KR-322.

**Decision 13 — `workers:` becomes `chunks:`.** On a pool the clause no longer starts anyone. `workers: 40` on 10 threads runs 10
at a time, and 40 is what balances an uneven machine (*measured* above), so the name would lie. Still mandatory, still capped by
length. **Sacrifice:** a rename across 26 fixture files and the docs; no peer has one. **Option: keep the word. Sacrifice:** a
name that says the opposite of what a reader needs. Rayon's `with_min_len`, Unity's `innerloopBatchCount` and TBB's grainsize each
state a size, not a count; kama keeps a count, since `cpuCount() * 4` is what is meant, and the size follows from length.

**Decision 14 — the pool in kama, and what that needs.** The ruling put it in kama code while every sibling seam is a C header.
*Measured* (`bench/jobs/pool_kama.kama`, the fork path on `Atomic`, `UnsafePtr` and a module `static`, with no C): fib(36) takes
**4.165 ms against the C arm's 4.151**, and 3.26 against 3.41 with one worker. **So the ruling stands, and it costs nothing
here.** It needs two things `Atomic` lacks: **`wait` and `notify`** (C++20's, on futex, `__ulock` and `WaitOnAddress`), which is
useful public surface anyway, and a seq-cst fence, which it can do without. The module loads only for a program holding a form, as
`spawnsFiber` gates fibers, or importing `Atomic` would start threads. **On macOS, no private symbol:** nothing in the tree calls `__ulock_*` today, and this would have been the first, in every binary with a
form, in the way of KR-281's App Review. **Recommended:** `os_sync_wait_on_address`, public since macOS 14.4 and iOS 17.4, weak-linked, and
under it a hashed table of `pthread` condvars, as libstdc++ does where it has no futex. **Sacrifice:** a slower wake on macOS 14.0-14.3
(*unmeasured*), and an arm CI's macOS 26 runs only when forced. **Option: raise the floor to 14.4. Sacrifice:** those users. The
prototype in `bench/jobs/` uses `__ulock_*`, which is fine for a bench. *Refined 2026-11-07 by step 2's W3 (below):* each thread sleeps
on its own word, or below 14.4 on its own condvar, queued in one table keyed by address.

**Decision 15 — the offload pool (step 7).**

| | threads | grows | |
|---|---|---|---|
| libuv | 4 by default, at most 1024 | no | shared by file and DNS work |
| tokio | up to 512 | yes, idle ones exit | "very large by default", for I/O that cannot be async |
| Java | spare threads up to 256 | by `ManagedBlocker` | |
| Go | up to 10,000 | yes | |

- **Spelling, for step 7's ruling: `spawn offload f(…)`, the same family and the same reason as decision 2,** as a scope child or
  a held `Offload<R>`. **Its line against `spawn` is the thinnest in the vocabulary:** on a fiber, a `spawn` child's join already parks
  (SPEC, `fiber_isolate_join`), so `spawn` already runs a blocking call off a fiber. What `spawn offload` adds is reuse and a cap, without
  which a thousand fibers each blocking on one call are a thousand threads. The pool itself is needed whatever this word is, for files and DNS. It may block on anything, since that is its purpose, and a fiber parks at the brace. It holds no job's
  rule; only `spawn fiber` is refused, since it has no scheduler.
- **Separate threads from the job pool,** since its work blocks. **Sacrifice, stated:** up to cores (I/O) + cores (jobs) + the
  offload pool's in one process.
- **Size: grows on demand to a cap, and idle threads exit,** tokio's shape: 512 on 64-bit, 64 on 32-bit and wasm,
  `setOffloadThreads(count:)` to change it. *Unmeasured:* the cap is a precedent, not a number from here.
- **Which calls: all of a regular file's, data and metadata,** on a fiber; DNS with KR-202; `Poller.wait` on AFD and `poll(2)`
  (the Windows box's). **A thread with no scheduler keeps calling inline:** nothing else could run.

**Decision 16 — `parallel_for` may bind a chunk, new 2026-11-05.** Per-chunk state is what a general data-parallel form lacks once statics go:
scratch memory, and any sum.

```kama fragment
parallel_for (ConstView<int32> part in xs, chunks: cpuCount() * 4) {
    int64 s = 0;                                       // this chunk's own, on its stack
    foreach (const ref int32 e in part) { s = s + cast<int64>(e); }
    total.fetchAdd(delta: s);                          // one add per chunk
}
```

*Measured:* a body binds an element today, so a sum is an `Atomic` add per element, because a write to a captured local is refused. Over
10,000,000 `int32` on the M4, `--release`, 7 rounds interleaved, min (`probes/parfor_atomic_sum.kama`): **a serial loop takes 0.57 ms, and
`parallel_for` with `Atomic` takes 268 ms, 468 times slower** (the serial loop vectorizes; the gap is not all contention). The binding does
not parse today (`probes/chunk_binding.kama.refused`).

| | a chunk | per-worker or per-chunk state | a reduction |
|---|---|---|---|
| Rust (Rayon) | `par_chunks_mut` | `for_each_init` | `fold`, `reduce` |
| C++ | TBB's `blocked_range` | `enumerable_thread_specific` | `parallel_reduce`, OpenMP's `reduction` |
| C# | `Partitioner` | `localInit` | `localFinally` |
| Go | a slice per goroutine | a local | by hand |

**Recommended: bind a chunk, and no reduction clause.** One binding answers scratch and reduction both, where a clause answers one and needs a
list of operators. std can then offer `parallelReduce` as an ordinary generic function over a functor, as `parfor_functor` shows. The
lowering already holds the slice (*predicted:* the same bytes, the body run once per chunk). **Sacrifice:** a second binding on one form, four
artifacts, and a result that follows `chunks:` if the body is sensitive to where a chunk ends. **Option: `reduction:` and `init:` clauses.
Sacrifice:** two clauses and an operator list, for less. Size S-M, and it belongs beside KR-322, which reshapes the same binding. Early exit
needs nothing new: `break` stays refused, and an `Atomic` flag with `continue` skips what is left.

**Decision 17 — a view may go to a scope child, and `splitAt` makes two, new 2026-11-05.** Recursive fork-join over data is the other half
of what a pool is for: parallel sort, merge, reduce. The brief's `fib` proves the pool does it, and **safe kama cannot write it over data:**
*measured,* a `View` is refused as a `spawn` argument, as a borrow for a window the child could outlive (`probes/view_to_spawn.kama.refused`;
until `0.9.859` it was refused for not being `Sendable` and not being given), and
by `ref`, since a view is already a borrow (`probes/view_ref_to_spawn.kama.refused`), and `View.slice` is `unsafe`.

```kama fragment
fn void sortPar(View<int32> v) {
    if (v.length() < 4096) { sortUnstable(v: v); return; }
    isize m = partition(v: v);
    borrow v.before(at: m) as l, v.after(at: m) as r {          // two disjoint views, safe: the spelling wants its design
        scope { spawn job sortPar(v: l); spawn job sortPar(v: r); }
    }
}
```

**Recommended: a view parameter on a scope child, its element `Sendable`, held to the prefix rule through the host it borrows; and a safe split
giving two views in one window.** Rayon's is `rayon::join` over `split_at_mut`; Java's `ForkJoinTask` passes indices over one array, with no
proof; Cilk spawns over pointers. For every `spawn` form, with decision 11. **Sacrifice:** the `borrow` window's rule that one host gives one
view grows a named exception. Size M?, *predicted*, and only the pool form needs it first. **Option: leave it out. Sacrifice:** a parallel sort is
`unsafe`, in std and in everyone's code.

#### The whole vocabulary, once this is built

| you want | reach for | runs on | may wait on | count known | what it costs |
|---|---|---|---|---|---|
| many things waiting at once: connections, timers | `spawn fiber` | this thread, taking turns | anything; a wait parks it | any, in a loop too | a pooled stack; about 8 KiB touched |
| one even split of a container | `parallel_for` | the job pool | the OS | at run time | nothing per hand-out |
| a few different CPU tasks, or a recursion | `spawn job` | the job pool | the OS | written in the block | nothing per hand-out; `Job<R>` a heap block |
| one blocking call, without stalling a fiber | `spawn offload` | the offload pool | anything | written in the block | a queue; `Offload<R>` a heap block |
| a thread of its own: a service, its own statics, a scheduler | `spawn` | a new thread | anything | written in the block; held as `Isolate` | a thread start |
| one thread per worker, running beside the code after it | `parallel_spawn` | a new thread each | anything | the container's length | a thread start each |

`scope { }` is every child's barrier. Between them: `Channel`, `Atomic`, `Locked`, `ReadWriteLocked`, `ConcurrentMap`. Six forms, **four spelled
`spawn`, where the first version of this brief said "a third `spawn` word" and did not add `spawn offload`.** Rust has about seven across three
libraries (`thread::spawn`, `thread::scope`, tokio's `spawn` and `spawn_blocking`, Rayon's `spawn`, `scope` and `par_iter`). Go has one, on
growable stacks and a runtime that adds threads, and sits behind the pool on barriers above.

#### What this does not close, and what it touches

**Not measured, and owed before any of this ships as a claim about general programs.** Every shape above is a short barrier or a recursion.
The build's gate adds: long jobs (seconds), where the pool should only match; two isolates sharing the pool; a job blocked on a file while
others wait; bare-metal Linux, and Linux on x86-64 (Windows on x86-64 is measured above).

| row | what changes for it |
|---|---|
| KR-79 reachability properties | gains a use: a proof that a job does no OS wait is its *no blocking syscall*. `@noparks` is one more hardcoded walk for it to absorb. The engine that asked would add one for replay: a simulation step reaches no outside input (no clock, file, socket, environment, `cpuCount()` or OS randomness) |
| KR-238 CET | a job's `stack:` inherits its refusal (decision 8) |
| KR-245 `@noheap` per isolate | unchanged in kind; a hand-out under `@noheap` on a fiber isolate parks at the brace, which is to be walked with it |
| KR-255 CPU bursts | closes on the rerun |
| KR-265 virtual clock | must not advance while a job or an offload is out, the trap tokio's paused clock has with `spawn_blocking` (*predicted*) |
| KR-296 cancellation | a cancelled fiber still waits at its brace, since a job holds its frame |
| KR-312 deadlock report | a fiber at a brace is not stuck while a job is out; a job blocked on the OS looks the same as one running |
| KR-322 `parallel_for` index | reshapes the same binding as decision 16: design them together |
| KR-233 fibers on bare metal, KR-19 | no threads there: `count: 1` is the natural meaning, unprobed |
| KR-24 hot reload | a held `Job<R>` may run code an unload removes (*predicted*); a `scope` child cannot outlive its brace |
| KR-281, KR-283 phones and headsets | P and E cores, and battery against a 50 µs spin, unmeasured; KR-281 needs decision 14's public wait |
| KR-35 VM | `Atomic.wait` joins what a second back end must provide; the pool itself comes along in kama |
| KR-25, KR-26, KR-309 the engine's packages | their consumers; nothing here names them |
| SPEC, `agents/AGENTS.md`, `docs/agents.md`, tree-sitter, the Zed and Helix pins | *Concurrency* rewritten, `workers:` gone everywhere, `spawn job` in every grammar |

Rows that stay independent of this brief: KR-254, KR-317, KR-297, KR-229, KR-202 (with step 7), KR-204. New from this review: `cpuCount()`'s Windows half (a defect, fixed in `0.9.855`) and KR-325 (a thread cannot be named, pinned or prioritized, and there is no verdict on `Condvar`, `Semaphore`, `Barrier` or `Once`). KR-326 (an arena-backed store cannot reach a task, which `spawn job` meets too) and KR-327 (no single-threaded wasm build of a program holding a form) carry the engine's answers from its audit (above).

#### Build order, once ruled

Each with fixtures on native, san, TSan and wasm, mutation-tested, behind a measured gate against the arms above.
1. Done: readers sharing a `const ref` (`0.9.858`) and a `spawn` over several places (`0.9.859`), each useful alone.
2. Done: `Atomic.wait` and `notify` (`0.9.871`). Next, the pool in `std::concurrent` and `parallel_for` on it, `chunks:`, decision 16's chunk binding, the `@noheap`
   proof and the `@onPanic` refusal.
3. `spawn job`: four artifacts and the pins, the walk, statics, `@noparks fnptr`, a fiber's park at the brace; then decision 17's views; then `stack:`.
4. KR-298; KR-315's element places; the held `Job<R>`.
5. Rerun `skew:12` with the burst offloaded, which closes KR-255, in its new shape: `scope { spawn job mix(b: ref buf, into: ref h); }`.
6. Then step 7, 10, KR-297, KR-296, 11, as ruled. KR-296 inherits a constraint: a cancelled fiber still waits at its brace, since
   a job holds its frame.

#### Step 2 begins: `Atomic.wait` and `notify`, ruled 2026-11-07

Decision 14 ruled where a sleeping thread's wait comes from:
- the public `os_sync_wait_on_address`, weak-linked;
- a condvar below macOS 14.4;
- no `__ulock_*`.

The surface itself is W1 to W6 below, **ruled as recommended on 2026-11-07**. The first draft had W1 to W5. Re-probing it
before the ruling changed four of them, and each correction is stated where it lands.

Measured by `bench/jobs/wait.c` and `bench/jobs/wait_cxx.cpp` (the C++ library as shipped) on an Apple M4 with macOS 26.4:
the median of 7 to 9 interleaved rounds. Linux and Windows are not measured yet; both files build on Windows, `wait.c`
with `-lapi-ms-win-core-synch-l1-2-0` under zig.

| arm, ns | a notify nobody waits on | a hand-off: a round trip, both threads awake | CPU beyond the table's on a 50 µs wait |
|---|---|---|---|
| libc++'s `atomic::wait`, as shipped | 3.2 | 68 | +9.3 µs |
| the OS's wake on the cell, every time | 94.7 | 1,807 | |
| the cell, plus a waiter count per bucket: libc++'s mechanism without its spin | 0.9 | 1,783 | |
| the table, no spin | 1.1 | 1,825 | 0 |
| **the table, polling the cell for up to 1 µs first (W6)** | **1.1** | **60** | **+1.7 µs** |
| the table, polling 4 µs, then yielding to 64 µs: the shape libc++ documents | 1.1 | 62 | +48.5 µs |
| the table, each thread asleep on its own condvar: below macOS 14.4 | 1.1 | 1,951 | |

**The first draft claimed parity with C++ from the cell-plus-count row, which never spins.** libc++ itself hands off 27
times faster than the table without a spin, because it polls before it sleeps. W6 answers that.

What it is for, the pool's idle worker and its fork:

```kama fragment
Atomic<uint32> epoch = Atomic.make(value: 0);
uint32 seen = epoch.load();
seen = epoch.wait(whileEquals: seen);     // a worker with nothing queued sleeps until the epoch moves
epoch.fetchAdd(delta: 1);                 // a fork
epoch.notifyAll();
```

| | wait | wake | timeout | cells | a green thread |
|---|---|---|---|---|---|
| C++20 `std::atomic` | `wait(old)`, no early return | `notify_one`, `notify_all` | none; P2643 proposes `try_wait_for` and `try_wait_until` (adoption not confirmed here) | every `T`, through a proxy word where the OS cannot wait | — |
| Rust | none in std; the `atomic-wait` crate's `wait(&AtomicU32, u32)` may return early | `wake_one`, `wake_all` | none | 32-bit | — |
| Zig `std.Thread.Futex` | `wait(ptr, expect)`, may return early | `wake(ptr, max)` | `timedWait` | 32-bit | — |
| JavaScript `Atomics` | `wait(ta, i, v, t)` answers `"ok"`, `"not-equal"` or `"timed-out"`; refused on a browser's main thread | `notify(ta, i, count)` | relative | 32 and 64-bit | — |
| Go | none on atomics | — | — | — | the runtime parks goroutines in a semaphore table keyed by address |
| Java, Swift | none on atomics; Java parks a thread with `LockSupport.park` | — | — | — | — |

**W1: the surface.** Ruled:

```kama fragment
public fn T wait(T whileEquals)                                          // the first other value it loads; never early
public fn Result<T, WaitError> waitUntil(T whileEquals, Instant deadline)
public fn void notifyOne()
public fn void notifyAll()
```

`WaitError` is `@nonExhaustive`, with the one variant `TimedOut`, so KR-296's `Cancelled` is additive.

- An early wake stays inside, as in C++, so a caller writes no loop.
- The value seen is returned, which saves the reload every caller makes.
- **The deadline is an `Instant`,** which is decision 11's ruling for sockets, for decision 11's reason: a relative
  timeout restarts on every call. `wait` returns at the first change, so a caller waiting for anything more specific
  loops by construction. With a `Duration`, this countdown would wait up to 50 ms for each worker that checks in:

  ```kama fragment
  Instant by = monotonicNow() + Duration.fromMillis(ms: 50);
  uint32 left = pending.load();
  while (left != 0) {
      match (pending.waitUntil(whileEquals: left, deadline: by)) {
          case Ok(value: now): { left = now; }
          case Err(error: e): { return Result::Err(error: e); }
      };
  }
  ```
- **It answers a `Result`, not an `Optional`.** A discarded `Optional` compiles without a word, while a discarded `Result`
  is refused (probed on `0.9.860`), so a timeout cannot go unseen. `recvTimeout` answers a `Result` too: the first draft
  said its `Optional` "follows `recvTimeout`", and it did not.
- There is **no `waitExplicit`**. The load that ends a wait is one instruction under either order: on Apple's arm64 it is
  `ldar` seq-cst and `ldapr` acquire, and on x86-64 a plain `mov` for both (compiled 2026-11-07). A weaker order would
  save one load's ordering next to a 1.8 µs sleep, which is inferred, not measured.

**Sacrifice:** `recvTimeout` takes a `Duration` and `waitUntil` an `Instant`, so a reader meets both shapes. A one-shot
timeout writes `monotonicNow() + d`, as a socket's deadline does. There are two waits where C++20 has one.
**Option: `Optional<T> waitTimeout(Duration)`. Sacrifice:** a timeout that can go unseen, and a loop whose bound
restarts at every change. **Option: every write notifies. Sacrifice:** a fence on every atomic write in every program,
which no peer pays.

**W2: every `Atomic<T>`, compared bitwise as `compareExchange` is.** That is the integers, `bool`, the floats and
`UnsafePtr`. The OS can wait on 4 bytes on Linux, 4 or 8 on Apple and wasm, and 1 to 8 on Windows. W3 makes the width
irrelevant, since no waiter sleeps on the cell. A change and a change back between two loads goes unseen, as in C++; an
epoch counter cannot meet that. **Option: 32-bit cells only, refused at the call as `fetchAdd` is on a `bool`,** as Zig,
Rust's crate and the futex do. **Sacrifice:** a `bool` flag or a 64-bit counter cannot be waited on, and a limit of one
platform becomes a limit of the language.

**W3: a waiter sleeps in one table keyed by the cell's address, never on the cell.**
- Every waiter, thread or fiber, queues in a bucket of a fixed table and sleeps on its own word.
- A notify reads the bucket's waiter count, and stops there when it is zero: a hash, a `dmb ish`, one load and a `cbz`.
- The table is 256 buckets of one cache line each, under `kama__xlock`: `os_unfair_lock`, `SRWLOCK` or a
  `pthread_mutex_t`. That is 16 KiB of zeroed static storage, which the emitter defines once in every hosted program
  that imports `Atomic`. It adds no bytes to the file and touches no page until something waits or notifies. The first
  draft said "only a program that waits", which overstated it.

Against libc++'s mechanism, the table costs 0.2 ns on an idle notify, and 2.4% on a round trip that sleeps. With W6, a
hand-off does not sleep. C++ waits on the cell because a thread is the only thing in C++ that waits. kama also has fibers,
which cannot sleep in the OS, and a cell wait would need a second path for them. Go's semaphore table, WebKit's
ParkingLot and Rust's `parking_lot` took the table for the same reason. Without the count, every idle notify is a system
call, which is why `pool.c` carries a `WAITER` bit of its own.

One mechanism answers four needs:
- a fiber (W4);
- every width (W2);
- macOS before 14.4: the word a thread sleeps on becomes its own condvar, 7% slower here. That was measured on 26.4;
  14.0 to 14.3 themselves are not measured;
- no private symbol.

**This refines decision 14 and does not reverse it.** The weak-linked `os_sync_wait_on_address` and the condvar beneath it
are still the two ways a thread sleeps. What changes is that the thread sleeps on its own word rather than the cell, and the
condvar is the thread's own rather than a hashed one.

**Option: wait on the cell where the OS can, use a proxy word elsewhere, and keep a separate list for fibers. Sacrifice:**
three paths, each with its own tests, to win 2.4% of a sleep.

**W4: on a fiber, `wait` parks the fiber.** On an isolate that runs fibers, the waiter record holds the fiber, on the
fiber's stack, and `notify` wakes it with `kama_sched_wake` from any isolate, under the bucket's lock, which is what keeps
the fiber alive through the wake. That is the channel's design (KR-3 step 6), with the cell's address as the queue.
Without it, a fiber waiting on a cell its sibling sets hangs the thread, since the sibling never runs. Go parks every
wait. Wasm has no fibers until step 10, so a waiter there is a thread. **Sacrifice:** a notify that finds a fiber pays
the scheduler's remote wake.

**W5: what the walks make of it.**
- `wait` is marked `@parks` at its seam, as every std wait is, so a lock window refuses it, as it refuses `recv`.
- It is a switch on a fiber (W4), so the park rule sees it.
- To decision 5's job walk it is a **task wait**, since only another task changes the cell, so a job that reaches it is
  refused, as `recv` is. `notify` waits on nothing.
- `@noheap` holds: the table is static, and a waiter record is on the waiter's stack, or its thread's on the condvar
  fallback.

**A wait that nothing could end, by target.** The first draft panicked on bare metal too, which was wrong: an `@interrupt`
handler changes a cell there, reaching it today through a `static UnsafePtr<Atomic<T>>` (probed `--target embedded`).
- **Wasm without threads:** a wait that would sleep panics, *"nothing in this program can change it"*. There is no other
  thread, no fiber and no interrupt, so the hang is certain. A notify is a no-op.
- **Bare metal:** it polls the cell. KR-233 makes the poll WFE and the notify SEV.
- **Native:** it sleeps. A wait that nothing can end hangs, as `recv` does, and KR-312's report is for both.
- **Stated residual:** on a browser's main thread, which may not block, emscripten's futex spins instead of sleeping.

**W6: a thread polls the cell for up to 1 µs before it queues; a fiber never polls.**
- The poll is bounded by time, reading the clock every 16 polls, with `isb` on arm64 and `pause` on x86-64. A process
  with one CPU skips it.
- A fiber never polls, since the code that changes the cell may be a sibling that cannot run while it spins. Go's
  `semacquire` does not spin either.
- *Measured:* a hand-off in 60 ns against libc++'s 68 ns, and 1.7 µs of CPU beyond the table's on a wait that does
  sleep, against libc++'s 9.3. Polls of 1, 2 and 4 µs all reach about 60 ns here, so 1 µs is the cheapest that does.
- The bound is a constant in the runtime header, recorded with this measurement, as libc++'s 64 polls, 4 µs and 64 µs
  are. It is not a parameter.

| | before it sleeps |
|---|---|
| libc++ | 64 polls, then polls to 4 µs, then yields to 64 µs |
| libstdc++ | 12 polls, then 4 yields |
| Rust std's futex `Mutex` | 100 polls |
| Go's `semacquire`, Java's `park` | nothing; a goroutine parks at once |

**Option: no poll. Sacrifice:** 27 times behind libc++ on a hand-off. **Option: libc++'s length. Sacrifice:** 28 times
the CPU on every wait that sleeps, which is the battery cost KR-281 names. **Option: a typed `spin:` parameter.
Sacrifice:** a choice no peer offers, at every call. A caller that wants longer polls `load()` first, as the pool does.

**Audit against GOALS.**
- **#1:** the pool, and KR-325's `Once`, `Semaphore` and `Barrier`, become kama code over it instead of C seams.
- **#3:** nothing new is shared; `Atomic` stays the one shared mutable cell. The storage is static or on a stack, and W4
  closes a hang.
- **#4:** one wait and one timed wait, with no ordering twin and no loop at the call.
- **#5:** a wait names what it waits on, returns what ended it, and shows its deadline at the call; a notify is written.

The production answer is C++'s surface with decision 11's deadline, over Go's mechanism, with a poll shorter than
libc++'s. *Measured:* parity or better than libc++ on the M4, for a hand-off and for an idle notify. *Unmeasured:* Linux
and Windows. **Size M:**
- the header, and the seam in `lib/std/concurrent/atomic.kama`;
- fixtures over threads, fibers, the widths, the floats and the deadline;
- the lock-window xfail;
- checks for the panic on wasm without threads, the bare-metal poll, and the weak link at the 14.0 deployment target;
- TSan, and a measured gate of the kama build against `wait_cxx.cpp`.

**Built in `0.9.871`; the gate, measured 2026-11-07.** `bench/jobs/wait_kama.kama` was run against `wait_cxx.cpp` and
`wait.c`'s `spin1000` and `table` arms on the M4, `--release` and `-O2`. Each figure is the median of nine interleaved rounds:
200,000 hand-offs, 20,000,000 idle notifies and 4,000 slow waits a round. CPU is from the children's rusage.

| | a hand-off, ns | an idle notify, ns | CPU per 50 µs wait, ns |
|---|---|---|---|
| **kama `Atomic.wait`** | **61.8** | **0.4** | **53,349** |
| libc++'s `atomic::wait` | 65.7 | 3.3 | 61,730 |
| the C model, `spin1000` | 57.7 | 1.2 | 53,485 |
| the C model, no spin | 1,840 | 1.2 | 51,679 |

Against libc++, kama is at parity or better on all three:
- a hand-off 6% faster;
- an idle notify at an eighth of the cost. It inlines into the caller, which the C arms' function pointers prevent;
- 8.4 µs less CPU on every wait that sleeps.

The hand-off is 7% behind the C model of the same table and poll; why is not measured. An
earlier run on the build before the rebase measured 65.9, 74.1 and 62.7 ns for the first three rows: this box drifts,
so only the interleaved comparison within one run is the claim. Linux and Windows are not measured yet. For Windows,
`wait.c` links under zig with `-lapi-ms-win-core-synch-l1-2-0` (measured); `-lsynchronization`, the Windows SDK's name,
does not link there.

What building it found, against the ruling above:
- **A bare-metal build cannot import `std::concurrent` at all.** That is filed as KR-341. Since `waitUntil` takes an
  `Instant`, every program importing `Atomic` also compiles `std::time`'s units. That is a few units more for a hosted
  build, and on bare metal it is the second half of KR-341. `kama_wait.h` itself reads no libc header beyond the
  clock's, and compares a cell by its width rather than calling `memcmp`. `check-embedded.sh` compiles it alone with the
  embedded flags, so it references nothing but `clock_gettime`.
- **`WaitOnAddress` is looked up, not linked.** The first build linked `synchronization` into every Windows program, and
  `check-target` and `check-windows-trims` failed: mingw-w64's kernel32 leaves the three functions out "for
  compatibility with older versions of Windows", and zig's mingw-w64 has no `synchronization` import library. They are
  resolved once from `api-ms-win-core-synch-l1-2-0.dll` through `GetModuleHandleW` and `GetProcAddress`, as libc++ and
  Microsoft's STL resolve them. A Windows build imports neither (measured on x64 and arm64 builds through zig). That the
  lookup succeeds at run time is for CI's Windows leg to show.
- **The table is 32 KiB on glibc's arm64,** whose mutex is 48 B and takes a bucket past one cache line. It is 16 KiB
  everywhere else.
- **The scheduler's park hooks are shared now.** The channel's `kama__channel_here`, `self`, `by`, `switch` and `wake` are
  `kama__park_*`, defined once in `kama_sched.h` for whichever of `kama_channel.h` and `kama_wait.h` asks.
- **A `notify` stays legal in a lock window.** Its bucket lock is a leaf, never held while another is taken, and
  `atomic_wait_threads` notifies from one. A `wait` is refused there (`xfail/atomic_wait_in_lock_window`).
- **Mutations.** Five were caught:
  - the count before the second look, which hangs the ping-pong;
  - the fiber path, which hangs `fiber_atomic_wait`;
  - a compare by value, which `atomic_wait_float_bits` answers wrongly;
  - `@parks`, without which the xfail compiles;
  - the deadline's path, which hangs both deadline fixtures.

  One was not: the notify's fence, which survived three runs. With the poll, a hand-off rarely reaches the queue. arm64
  lets a store pass a later load of another address, as x86's store buffer does, and that reorder is the lost wake the
  fence prevents, but three runs on the M4 never showed it. TSan does not model fences, so no leg here can catch it.
  The fence is what the C11 model requires. **Stated residual.**

<a id="s6-thread-control"></a>

### Thread control and the remaining primitives (KR-325)

Nothing in `lib/` or `include/` names a thread, pins one or sets its priority or QoS class: no `pthread_setname_np`, no affinity call, no
`pthread_set_qos_class_self_np`. Rust names threads through `Builder::name`, Go has `LockOSThread`, C# has `Thread.Name` and `Priority`, and a
debugger, a profiler and `top` all read a name. It matters more with a pool: on P and E cores a chunk on a slower core holds the barrier
(`s6-pools`), and on Apple silicon QoS is what picks the core. **Verdict: scheduled, design first,** after the pool, so the pool's own threads
are named and classed by it.

There is also no `Condvar`, `Semaphore`, `Barrier` or `Once` in std, and no written verdict. SPEC's *Channels* says a channel is the seam, in a
sentence older than `Locked`. A bounded channel is a semaphore, and `scope` is a barrier for its own children. `Once` has no answer short of an
`Atomic` by hand, and `Atomic.wait` (decision 14) makes each of them writable in kama. **Verdict owed, per primitive.**

**The engine that asked (2026-11-05)** needs names for its profiles of frame time per phase, and priority and core class, since a chunk on a slower core holds the
barrier on Quest, phones and Apple silicon. Not looked at: how a native audio backend's real-time thread calls into kama.

<a id="s6-arena-task"></a>

### A store drawn from an `Arena` cannot be handed to a task (KR-326)

Measured `0.9.850` (`bench/jobs/probes/arena_to_spawn.kama.refused`, `arena_parfor.kama`). Two `FixedArray<int32, BumpAllocator>` from one `Arena`, each borrowed by
its own `spawn`, are refused at the bundle gate: `BumpAllocator` is not `Sendable`. The refusal is sound, since a `BumpAllocator` is a value holding a raw pointer to its
arena's offset and bumps it unsynchronized, and a child holding the store could grow it. A `parallel_for` over the same store checks, because a body reaches elements and not
the allocator. So work split over an arena's store is fine, and work handed the store as a whole is not. Budgeting memory per region, sized when the region loads and released
in O(1), is the case that wants it.

| | an arena several tasks may use |
|---|---|
| Rust | `bumpalo::Bump` is `Send` and not `Sync`; `bumpalo-herd` gives each thread a member |
| C++ | `monotonic_buffer_resource` is unsynchronized; `synchronized_pool_resource` is another type |
| Zig | `ThreadSafeAllocator` wraps any allocator |

**Two shapes, by whether a task allocates while it runs.** An atomic bump lets tasks allocate, and an uncontended bump pays an atomic add, so it wants its own type rather than a
slower `BumpAllocator`. A frozen handle, made once a region's stores are sized, may only free (a no-op), which makes it `Sendable` and costs nothing, and a task can never
allocate through it. **Verdict: scheduled, design first,** with the question to the engine that reported it: which shape does its work need? Before `spawn job`, whose `ref`
parameters meet the same gate.

**The engine's answer (2026-11-05): both, each for a case it has decided.** A frozen handle for tables sized when a region loads and never grown, each run as its own task;
an atomic bump for per-entity data and lists that spill past their capacity, in chunks that grow inside a worker, and later cells generated off the simulation. The second
is refused under `parallel_for` as well (measured `0.9.852`): a `type resource ItemChunk { DynamicArray<int32, BumpAllocator> overflow; … }`, eight drawn from one `Arena`
and grown by `parallel_for (ref ItemChunk c in chunks, workers: 8) { c.step(); }`, is refused at the element, "`ItemChunk` does not declare `implements Sendable`", and
declaring it is refused at the field. Sound, since eight workers would bump one offset. So data that grows inside a worker splits across workers only on the global heap,
outside the region's budget. Both shapes are allocator types the containers already take (`DynamicArray<T, A>`, `FixedArray<T, A>`); nothing in the pool changes.

<a id="s6-wasm-serial"></a>

### A wasm program holding a thread form has no single-threaded build (KR-327)

Measured `0.9.850` (`bench/jobs/probes/wasm_form.kama`, `wasm_noform.kama`, `--target wasm --release`): one `parallel_for` makes the build threaded, the JS naming
`SharedArrayBuffer` and `PThread` at 21,544 bytes, against neither at 8,893. The driver reads `spawnsThreads()` for it: a threaded runtime is a hosting model, chosen by what the
program holds. A browser gives `SharedArrayBuffer` only to a cross-origin isolated page, so that page must send COOP and COEP, which not every host can, and `main` moves to a
worker. A library using `parallel_for` inside therefore decides its host's headers, and a program that wants threads only by a switch must gate its forms out at build time.

Rayon's global pool falls back to the current thread where threads are not supported, so the same source runs on wasm without atomics. **On a pool the answer is close to free:** a
setting under which there are no worker threads, `parallel_for` and `spawn job` run on the caller in order, as `setJobThreads(count: 1)` already does at run time, and no threaded
runtime is linked. `spawn` and `parallel_spawn` mean a thread, so they stay refused there. **Ruling owed, with KR-3's step 10:** whether this is a per-build setting and how it is spelled. *Predicted*
to need no change to a form's lowering beyond the pool's own arm.

**The engine that asked (2026-11-05): a per-build setting.** Its web build is single-threaded by default and takes threads from a build-time switch a game turns on by
measurement, from one source. Set by the program being built, never by a dependency, which is the rule `no-heap` and `webgpu` follow: a library using `parallel_for` inside
must not decide its host's headers.

<a id="s6-parfor-index"></a>

### `parallel_for` over two stores, or with an index (KR-322)

Filed 2026-11-04, from a game engine on kama and seen again writing the pools' brief. `parallel_for` binds an element and never
its index, so a system that reads one store and writes another in step (`pos[i] += vel[i] * dt`) needs both in one element, or a
chunk type holding both columns, which is what the reporter does. C#'s `Parallel.For` and OpenMP hand the body an index; Rayon has
`zip` and `enumerate`. An index alone does not fit kama: `vel[i]` beside a `ref` to `pos[i]` would need the same proof as KR-315.
Two operands split alike (`parallel_for (ref Pos p, const ref Vel v in pos, vel, chunks: n)`) carry the proof with them, and
differing lengths would have to trap. **Verdict: scheduled, design first,** after the pool, since it changes that form's lowering.

The engine that reported it no longer needs it for its own tables (2026-11-05), since it now keeps each table's columns in one chunk. The verdict stands on its
own: a data-parallel loop over two arrays in step is common, and C# and OpenMP give it an index.

<a id="s6-job-priorities"></a>

### Job priorities (KR-320)

Filed 2026-11-04, writing the pools' brief, which leaves them out on purpose. A held `Job<R>` that runs for milliseconds occupies
a worker a frame's barrier may be waiting for, and one queue cannot say that the barrier matters more. TBB has `task_arena`
priorities and Unity keeps long work off its job threads by convention. **Verdict: scheduled, design first,** once a pool exists
to measure: how often it happens, and whether a second set of threads for long work answers it more simply than priorities do.

**The engine that asked (2026-11-05): its long jobs arrive with the held `Job<R>`, so design this in KR-3's step 4, not after a later measurement.** Streaming a region,
generating cells and building a navmesh run off the simulation for several frames while every frame's barriers wait for workers, and for a procedural world that is
its first day. It needs a barrier never to wait behind such a job; a second set of threads for long work would answer it.

<a id="s6-fiber-cet"></a>

### Fibers under CET shadow stacks (KR-238)

**Found building KR-3 step 1 (`0.9.737`).** fibers.md decision 2 gave the x86-64 switch a `__CET__` arm "so a hardened
distro's default `-fcf-protection` still links". It links, and that is not enough. `-fcf-protection=full` marks the
object shadow-stack compatible, and a process whose every object is marked may run with shadow stacks once the kernel
(Linux 6.6+) and glibc enable them. The hardware then checks every `ret` against a second stack the switch never
touches. The first `ret` into a new fiber's boot stub would fault. Since `0.9.737` the switch reads the shadow-stack
pointer with `rdsspq`, which is a no-op leaving zero where shadow stacks are off, and refuses with a message where
they are on.

**What supporting them takes.** Boost.Context's x86-64 CET arm is the reference:
- each fiber gets a shadow stack of its own, from the `map_shadow_stack` system call, with a restore token at its top;
- the switch does `saveprevssp` / `rstorssp` beside the `rsp` swap;
- a fiber's shadow stack is unmapped with its stack.

It is unprobed here: this box is arm64 and its container is too, so the work wants an x86-64 Linux host with a
CET-capable CPU and a kernel and glibc that enable it. Size M?. BTI and PAC on arm64 need nothing (fibers.md
decision 2).

<a id="s6-noheap-per-isolate"></a>

### `@noheap` reach per isolate (KR-245)

**Found 2026-10-26, planning KR-3 step 5.** The `@noheap` walk reads one call graph for the whole program. On an
isolate that runs fibers, a socket's first wait registers it with the scheduler, which allocates the record; on an
isolate that runs none, the same call does not. The graph cannot tell the two apart, so in a program that runs fibers
anywhere, `@noheap` code that does socket I/O or calls `sleep` is refused on every isolate, and since step 6
(`0.9.751`) so is a channel wait or a join, which reach the scheduler's timer heap and its loop. A program runs fibers
when its code holds a `spawn fiber`; a program that imports `std::concurrent` only for a channel is not refused. The
park rule already
walks per isolate (its roots are the entries whose reach includes a fiber spawn, `checkParkWindows`), so the shape
exists.

**Accepted as built 2026-10-26** (fibers.md decision 15): the refusal is conservative, never unsound, and moving the
`@noheap` boundary out of the I/O call answers it. The precise answer — judging an allocation by the isolates that
reach it, as the park rule does — is this row, beside KR-79's generalization of the same engine.

<a id="s6-virtual-clock"></a>

### A virtual clock for the fiber scheduler (KR-265)

Filed 2026-10-30. The scheduler's timers, `sleep`, timed receives and socket deadlines all read `kama_now_mono_ns()`
directly (`kama_sched.h` ~369, ~879, ~909, ~1030; `kama_os.h` ~4335), so a test of a timeout waits in real time.
Go's `testing/synctest` runs a bubble whose clock jumps to the next timer once every goroutine in it is durably blocked;
tokio's `time::pause()` freezes the clock, which `advance` moves and the runtime moves itself when idle. **Design
first:** a scope with its own clock that jumps when every fiber in it is parked on something the scope owns (a channel,
a timer, an in-memory stream, never a real socket), and how `monotonicNow()` reads it inside.

**What it inherits.** A `MemoryStreamPair` parks and takes deadlines since `0.9.844`, so a timeout over one is already a real
wait on the scheduler's clock. Go's `testing/synctest` (1.25) uses `net.Pipe` as its fake connection, since a goroutine blocked
on a real socket never counts as idle; its page does not say whether a pipe's deadlines follow the fake clock, so read
`net/pipe.go` and try it before leaning on Go here.

**What a wait costs, measured 2026-11-04 by `bench/pipe/`** (`0.9.844`; nanoseconds, the minimum over 9 interleaved rounds, an
M4 Mac and a Linux VM on it, kama on fibers, tokio 1.54 on one thread, Go 1.27 at `GOMAXPROCS=1`). `tokio` has the I/O driver
that `#[tokio::main]` and `#[tokio::test]` turn on, every side a spawned task, and "no driver" is the runtime built with neither
sockets nor timers:

| | kama | tokio | tokio, no driver | Go | | kama | tokio | tokio, no driver |
|---|---|---|---|---|---|---|---|---|
| | **Linux** | | | | | **macOS** | | |
| ping-pong round trip, 64 B | **63** | 92 | 70 | 759 | | **69** | 462 | 76 |
| ping-pong round trip, 4 KiB | **200** | 243 | 195 | 833 | | **226** | 610 | 223 |
| 8 KiB chunk through an 8 KiB ring | **212** | 270 | 214 | 586 | | **216** | 622 | 214 |
| 64 KiB chunk through an 8 KiB ring | **1,836** | 2,444 | 1,690 | 1,986 | | **1,766** | 4,843 | 1,725 |
| write and read each way, nothing parks | **18** | 48 | 34 | no buffer | | **22** | 394 | 39 |

kama leads every row against Go and against tokio with its driver. Against a runtime that cannot do I/O it is even, except on
Linux 3% behind at a 4 KiB ping-pong and 8% at 64 KiB chunks. One side in `block_on`, the body of a `#[tokio::test]`, costs tokio
318 ns a round trip on Linux and 11.7 µs on macOS, about what a `kevent` call costs there (read from the size, tokio not profiled).

The first measurement read the other way: 507 ns a round trip against tokio's 456 on macOS, 60% of it one `kevent` call every 61
runs (about 10 µs each, from that share of the profile). Two scheduler changes in `0.9.843` closed it, fibers.md decision 12 *As built*. The ring's own cost is
what the feature adds where no scheduler runs: 19.9 ns a pass against 16.8 before it, the deadline and waiter tests.

<a id="s6-deadlock-report"></a>

### Every fiber parked for good hangs in silence (KR-312)

Filed 2026-11-04, measured on `0.9.842`: `main` joining a fiber that receives on a channel nobody sends to prints nothing
and never returns. Go's runtime ends such a program with "all goroutines are asleep - deadlock!", and tokio hangs as kama does
(both from their documentation, not run here). An in-memory pipe makes the case common: `MemoryStreamPair` exists for hermetic tests, and
a protocol test with a bug would then hang its suite rather than fail it.

**The verdict: scheduled, design first.** A scheduler knows its own fibers, so the question is what else could end a wait: a
sender on another isolate, a socket, a timer, a child process. A report is true only when none of those exists on any isolate,
which is a whole-program question on a runtime that is per-isolate by design. KR-265's "every fiber is parked on something the
scope owns" asks nearly the same thing of one scope, so the two want designing together.

<a id="s6-disjoint-parts"></a>

### Parallel tasks over parts chosen at run time (KR-315)

Reported 2026-11-04 by a game engine on kama; reproduced on `0.9.844`. With `DynamicArray<DynamicArray<Pos>> cols`, `scope { spawn
step(col: ref cols[0]); spawn step(col: ref cols[1]); }` is refused at both spawns: an element "names no place a static disjointness
rule can pin", by design (SPEC). `parallel_for` covers an even split of one buffer, not a chosen part per task, which is what a scheduler
wants when load or region picks the columns, zones or chunks. Rust's `slice::get_disjoint_mut` proves the indices distinct when it runs
(from its documentation, not run here).

**The verdict: scheduled, design first, in KR-3's pool brief.** Safe, with the check at run time: the shape of the split, what a `spawn`
may borrow from it, and whether its parts can be places the prefix rule already understands.

<a id="s7"></a>

<a id="s6-fiber-cancel"></a>

### Cancelling a fiber (KR-296)

Filed 2026-11-02. KR-3's decision 11 ([docs/design/fibers.md](design/fibers.md)) says cancelling a scope marks its fibers, and
each one's current and every later park returns `Err(IoError{Cancelled})` until it ends. It is unbuilt: `Cancelled` appears
nowhere in `lib/std`, `include` or `src` on `0.9.811`, and the design's implementation map gives it no step.

A server with a fiber per connection has to end parks it did not write: evicting a connection at the cap, graceful shutdown,
a request the application never answers, a client gone while its request is held. Reported workarounds, not re-run here:
`shutdown` on a duplicate descriptor wakes a parked read, which costs a descriptor per connection and withholds the FIN
until the duplicate closes; a connect to its own port wakes a parked `accept`; slicing deadlines costs a wake per slice per
connection. Nothing wakes a fiber parked on a `sleep` or a channel with no timeout, so a handler that never answers holds its
fiber and its scope's closing brace.

Go closes a `net.Conn` from any goroutine (`net.ErrClosed`) and cancels a request's `context.Context`; tokio cancels at the
next `.await` when a future is dropped; .NET passes a `CancellationToken` to every wait. Wanted, as designed, in two forms:
one fiber, by a `cancel()` on a handle, which a bare spawn does not give back today; and a whole scope. Which handle a bare
spawn gives back wants a ruling first.

Not every park answers a `Result`. `recv` answers `Optional<T>`, `sleep` returns nothing, and `Atomic.wait` returns the
value it saw (KR-3 step 2, W1), so none of them has an `Err` to carry `Cancelled`. They want one ruling together. The
timed `Atomic.waitUntil` answers `Result<T, WaitError>`, and `WaitError` is `@nonExhaustive`, so `Cancelled` is additive
there.

<a id="s6-fiber-yield"></a>

### `std::concurrent::yield()` (KR-297)

Filed 2026-11-02. KR-3's decision 12 says "`std::concurrent::yield()` for a long loop". It is unbuilt on `0.9.811`: nothing in
`lib/std/concurrent` declares it, and `sleep` of a zero span returns without parking, as its comment says. A fiber gives way
only at a park, and a socket call parks only when it would otherwise block, so a client with pipelined requests already
buffered, or a large body to a fast client, runs while every other fiber of its isolate waits. The workaround is a budget and
a one-nanosecond `sleep`, a trip through the timer heap where a `yield` would be one run-queue push. Off a fiber it should be
a no-op. Go preempts; tokio budgets 128 operations per task inside its resources, so no loop has to think about it, and that
is the larger question to rule: a `yield` alone, or a budget in std's socket calls as well.

<a id="s6-channel-try-send"></a>

### `Sender.trySend` (KR-298)

Filed 2026-11-02. `Receiver` has `tryRecv` and `recvTimeout`; `Sender` has only `send`, which parks (`lib/std/concurrent/channel.kama`,
no `trySend` on `0.9.811`). A broadcaster with one bounded channel per client therefore waits for its slowest reader. Wanted:
`trySend(item:)`, which never parks and hands the item back when the channel is full or closed, as `Undelivered` does today.
Go's `select` with a `default`, tokio's `try_send` answering `Full(value)`, crossbeam's `try_send`. The reported workaround is
a counter kept by hand on both sides of every channel.

**A second reason (2026-11-05):** a job may not park (KR-3's decision 5), so `send` is refused in one and `trySend` is the only send a job has. A game engine on
kama needs it for its output phase's hand-off to audio and the network, whenever that phase runs on the pool. It is in KR-3's build order, step 4.

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
scene/material. Depends on the 1.x systems (file I/O for assets, serialization for scenes). The scene, its meshes,
materials, lights and cameras are `@kama/graphics`, a first-party package since 2026-11-04 (below), and the engine
product is what is left above it: frame pacing, its systems and the XR swapchain. See
[ENGINE_READINESS.md](ENGINE_READINESS.md).

**GPU and window are first-party PACKAGES, not std — decided 2026-10-16.** The maintainer's condition: "if it is in
std it must be general purpose and exhaustive". No systems language ships either in its standard library (Rust has
the wgpu and winit crates; Odin keeps them in `vendor:`, outside `core:`; Zig, Go, C, C++ and Swift have none; Java's
AWT and Python's tkinter are the exceptions). Both stand on third-party SDKs (wgpu-native, GLFW), and webgpu.h still
breaks between releases, so std's 1.0 promise would follow someone else's release cycle. This is the HTTP precedent
(§1, 2026-10-14): std carries the platform's transports and building blocks, and a first-party package carries the
library on top. So `@kama/gpu` (KR-25) and `@kama/window` (KR-26) are packages, built in their own repositories; the
decisions below were ruled while they were std rows and travel with them. **No consumer uses the std seam today:** the
downstream engines bind `<webgpu/webgpu.h>` and their own GLFW platform layer, because the seam's window is a file
static that input cannot be attached to — so nothing waits on the packages, and those layers are their reference
consumers when they are built.

**A downstream engine builds both — 2026-11-02** (KR-25 and KR-26 are its claim). Its sessions build each in its own
repository, to the designs below and from the platform layers it replaces, and it is their first consumer. The
packages stay first-party and these designs stay kama's: a change the build needs comes back as an amendment of its
section here, and a compiler or std gap the build meets is filed as a row. What stays in the engine is what the
designs already leave out of them: frame pacing and the XR swapchain (the renderer went to `@kama/graphics`, below).

**A graphics library is a first-party package too: `@kama/graphics` — ruled 2026-11-04** (KR-309). Scenes, meshes,
materials, lights, cameras and loaders, on `@kama/gpu`, naming no engine concept. It stands alone by the rule below:
a model viewer, an editor and a non-game product each use one, and none needs a frame loop, a simulation or real time,
as a tool needs GPU and window as much as a game does. Unlike KR-25 and KR-26, **its design lives in its own
repository** (`cosmic-canopy/kama-graphics`, seeded 2026-11-03), not here, and the downstream engine that seeded it
builds it and is its first consumer. A compiler or std gap the build meets is still filed here as a row.

**XR is the engine's: neither std nor a first-party package — ruled 2026-11-02** (filed the day before as `@kama/xr`). The
maintainer's rule: a capability is the language's or std's if it belongs there, a first-party package if it stands
alone, and otherwise the engine's. GPU and window stand alone: the downstream games each wrote their own WebGPU binding
and GLFW layer, and a tool or an editor needs both as much as a game does. XR does not, yet. Its one consumer is an
engine, and its hard half is engine design: the swapchain handed to the renderer (Vulkan or GLES on Quest; whether
wgpu-native exposes either is unprobed) and, on the web, the frame loop `XRSession.requestAnimationFrame` owns. One
interface over OpenXR and WebXR is an engine abstraction (Godot's `XRServer`). A bare OpenXR binding can stand alone, as
Rust's `openxr` crate does, so the engine may publish one later. What kama owes XR is Quest's Android route: the
target shipped in `0.9.830`, and the app is KR-283; a gap the engine meets past that is filed here as a row. The
2026-11-01 survey, for that engine: Quest's loader is Khronos's `openxr_loader_for_android` AAR (Meta supports
1.0.34 and later), initialized with the activity's `JavaVM`
and `jobject`; on Windows it is `openxr_loader.dll` with Quest Link as the runtime, so a desktop build is tested in the
headset before the Android app exists; WebXR is reached through `jsLibraries` with an `expose fn` as the per-frame
callback, only on the page thread (a threaded wasm build's `main` runs on a worker), and renders through WebGL2, since
WebGPU in WebXR is behind a flag in the Quest browser.

**What stays in the toolchain: `--webgpu` / `"webgpu": true`** (docs/targets.md). It is the platform half — it finds
the wgpu-native SDK (`tools/fetch-webgpu.sh` or `$KAMA_WGPU_DIR`, with an actionable error when it is missing), adds
its include, library and rpath, and on wasm passes emscripten's own `--use-port=emdawnwebgpu`. Without it every
project would hardcode SDK paths and emcc switches. A dependency cannot turn it on for its consumer, by design, so
`@kama/gpu` should refuse to build — naming the key — when the consumer has not set it, rather than fail on
"webgpu.h not found". The seam itself left std in `0.9.652`: it is `examples/webgpu`'s own platform layer now
(`csrc/gpu.*`, its `csources`, gated by fact), and the driver knows nothing about it.

- **The safe GPU binding (`@kama/gpu`, KR-25) — no longer "optional polish".** It was filed as `std::gpu`, over a
  C seam with *no kama at all*. The wrapper is the missing kama file, shaped like `std::net`'s `type resource TcpStream
  { isize fd; }`: `Device`, `Surface`, `Buffer`, `Texture`, `TextureView`, `RenderPipeline`, `BindGroup`,
  `CommandEncoder`, each an `UnsafePtr` handle whose destructor calls the matching `wgpu*Release`. Plus a
  `Result<SurfaceTexture, SurfaceError>` acquire whose error enum **names `Occluded`**, and the two seam
  holes below (drawable size; the discarded event queue).
  - **Why it was promoted.** A WebGPU frame loop with unbounded allocation — one that
    reached 15.6 GB and took a machine down twice. ⚠️ **Be precise about which half would have caught it:**
    RAII would *not* have. That allocation is inside wgpu-native, triggered by ignoring an untyped status
    int; what prevents it is the **typed acquire result**. RAII prevents the *other* leak in the same file
    — `examples/webgpu/triangle.kama` hand-writes **five** `wgpu*Release` calls per frame, all on the happy
    path, so any early `return` added later leaks them. Two different bugs, two different halves.
  - **Scope.** It starts at exactly the handles the triangle touches; as a package it grows toward the whole of
    webgpu.h. Buffers/bindings/pipelines *as a renderer* stay out, since that is `@kama/graphics` on top of it; the
    same things *as released handles* are in.

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

<a id="s8-gpu-wrapper"></a>

### `@kama/gpu` — the safe WebGPU binding, a first-party package (KR-25) — surveyed on `0.9.630`

A PACKAGE since 2026-10-16 (§8's opening). The survey below was written for a std module; its design and decisions
carry over unchanged, and its "manifest entry" / `check-gpu.sh` steps become the package's own manifest and CI.

The current state:

- The seam is `examples/webgpu/csrc/gpu.h` (the web half, static inline) and `gpu.c` (native, GLFW; macOS compiles
  it as Objective-C through `gpu_macos.m`) — the example's own `csources` since `0.9.652`, with no kama code.
- It is four functions: `gpu_surface` (it creates the window and the surface together; the native window is a file
  static `g_window` that is never destroyed), `gpu_present`, `gpu_pump` and `gpu_sleep_ms`.
- A native `kama build examples/webgpu/kama.json` (wgpu-native v29.0.1.1, GLFW 3.4) succeeds, printing the two
  documented fnptr warnings. The smoke binary prints "WGPUInstance created". `tools/check-webgpu.sh` checks both for
  native and wasm on every run without the SDK, and builds them where it is installed.
- The triangle leaks more than its five per-frame releases cover: the shader module, the `GetBindGroupLayout` ref, the
  adapter, the instance and the calloc'd `App`. It also hardcodes 512×512 BGRA8.
- webgpu.h is not one header: the web's (emdawnwebgpu) and wgpu-native's differ in what each declares. A project that
  generates its bindings from both found 199 functions shared, none declared differently, and gates what only one has
  (reported 2026-10-30); the binding generates from both and gates the same way.
- webgpu.h ref-counts all 23 handle types (`Release`/`AddRef`). Releasing the last Device ref destroys it, so its
  children turn INVALID (validation errors) rather than dangling.
- The web leg is unprobed: there is no emcc on the host.

Sites:

- The package's own `csrc/` takes over `examples/webgpu/csrc/gpu.*` (surface, pump, the web bodies), and its own
  `kama.json` the gated GLFW/framework links the example's carries.
- Its kama module, `gpu.kama`.

The design:

- **One `type resource` per handle:** Instance, Adapter, Device, Queue, Surface, ShaderModule, RenderPipeline,
  BindGroupLayout, BindGroup, Buffer, Texture, TextureView, CommandEncoder, RenderPass, CommandBuffer and
  SurfaceTexture.
  - Each holds a private `UnsafePtr` whose `~T` calls `wgpu*Release` (the `Poller` shape).
  - Each has `public unsafe ctor adopt(UnsafePtr raw)` and `unsafe fn raw()`, so an engine can bind more of the API
    with its own externs.
- **The once-in-order steps are moves:** `encoder.finish()` consumes the encoder, `queue.submit(give cmd)` the command
  buffer, `pass.end()` the pass, and `frame.present()` the frame. Release always follows present, and a frame dropped
  without presenting is skipped.
- **A typed acquire:** `surface.acquire() -> Result<SurfaceTexture, SurfaceError>`, with `SurfaceError { Occluded,
  Outdated, Lost, Timeout, OutOfMemory, Error }`. Suboptimal is a success flag on the frame. The seam maps wgpu.h's
  `0x00030001` to `Occluded`; the web never produces it, so the triangle's `@compileFor` goes away.
- **The async device, without closures or ASYNCIFY:**
  - The C seam chains the adapter and device callbacks into a heap slot, published with a release-store (a callback
    may fire on a wgpu thread).
  - kama polls: `instance.requestDevice(surface:) -> DeviceRequest`, and `poll() -> DeviceStatus { Pending,
    Ready(Device device, Queue queue), Failed(GpuError error) }`.
  - The user-side fnptr warnings go away.

The commits:

1. Seam rework; `gpu.kama` with Instance, Adapter, Device, Queue, Surface, DeviceRequest and the typed acquire; the
   package manifest.
2. The remaining handles, with safe creation for exactly what the triangle uses.
3. The triangle on `@kama/gpu` with zero `wgpu*Release` calls; its README and ENGINE_READINESS.
4. The package's CI: `kama check` its tests, native and wasm, always (no SDK needed); build, link and run them where
   the SDK (or emcc) exists. Plus an xfail for a frame used after `present()`.

Traps:

- On the web `run` never returns, so main's resources are never dropped. That is required, not a leak.
- emdawnwebgpu aborts on an explicit present, so `present()` stays a seam call.
- Take enum numbers from the header; never write them by hand.
- `--webgpu` stays an explicit flag.
- `Surface.configure` needs the framebuffer size, which KR-26 supplies.

Size **L** (4 commits).

**Decided 2026-10-16** (the maintainer took all four):

1. A handle's copy: **`Copyable(bare: give)` with copy = `AddRef`.** It is webgpu.h's own model, and it
   avoids a double count through `Shared<Buffer>`. The alternative is move-only.
2. Parent lifetime: **independent handles.** Dropping the Device destroys it, which is WebGPU's semantics
   and stays memory-safe. The alternatives: every child AddRefs its device, or `Shared<Device>`.
3. Descriptor breadth: **safe creation for the triangle's set first, plus `adopt`/`raw`.** As a package it grows
   toward the whole of webgpu.h release by release, without std's stability promise.
4. `Occluded`: **`acquire` sleeps 32 ms natively before returning `Err(Occluded)`,** the way a browser
   throttles a hidden page. There is one correct response, and that failure once took a machine down. The alternative
   is to leave it to the caller.

<a id="s8-input"></a>

### `@kama/window` — a window and its events, a first-party package (KR-26) — surveyed on `0.9.630`

A PACKAGE since 2026-10-16 (§8's opening); the survey was written for a std module, and its design carries over.
Its commit 0, a compiler defect (a bound plain enum refused as a `match` subject), shipped in this repo as `0.9.634`.

The current state:

- The example's native `gpu_pump` calls `glfwPollEvents` with no callbacks registered, so only the close flag survives.
- The web pump is `return 1`, with no html5 listeners.
- No key, mouse, size or focus information exists anywhere.
- A pull-API prototype worked: a C ring queue, `type enum InputEvent`, `pollEvent() -> Optional<…>`, and
  `try cast<Key>(n)` falling back to `Unknown`. It needed a bound key to be matchable, which `0.9.634` made true.
- A downstream engine writes its own GLFW/html5 platform layer today, because the seam's window is unreachable and its
  events are discarded. This module must make that layer unnecessary: several windows, size and scale, and a native
  handle.

Sites:

- The example's `csrc/gpu.c`: `gpu_pump` and `g_window`; `csrc/gpu.h`: the web pump.
- The package's own `window.h`/`.c` in `csources`, and its manifest's gated GLFW links (the flag gates of `0.9.651`
  are what let a package say macOS's frameworks without the compiler's help).

The design:

- **A `Window` resource.**
  - Native: a GLFW window with its own ring queue, reached through the window user pointer, so several windows work.
  - Web: it binds `#canvas` and registers html5.h callbacks, so no JS library is needed.
  - NOT `Sendable`, which enforces GLFW's main-thread rule for free.
  - Methods: `open(title:, width:, height:) -> Result<Window, WindowError>`, `pump()`, `pollEvent() ->
    Optional<Event>`, `size()` (logical), `framebufferSize()` (pixels), `scale()`, `setCursor(mode:)`, and
    `unsafe nativeHandle()`.
- **`Event`:** `KeyDown(key, mods, repeat)`, `KeyUp`, `Text(char c)`, `MouseMove(x, y, dx, dy)`, `MouseButton(button,
  down, mods)`, `Scroll(dx, dy)`, `Resize(w, h)`, `ScaleChanged`, `Focus(bool)`, `PointerLock(bool)` and
  `CloseRequested`. One queue keeps a resize and a click in order.
- **`Key`:** one physical-key enum, with USB HID usage ids as the discriminants. That is what DOM `KeyboardEvent.code`
  names, and what GLFW's US-named, layout-independent tokens mean. Two C tables translate into it; anything unmapped
  becomes `Unknown`.
- **Coordinates** are logical, and `scale()` converts them.
- **The queue** is a fixed ring (no heap) that coalesces moves, scrolls and resizes.
- **Gamepad** comes later. GLFW's gamepad API and `getGamepads()` both expose the W3C standard layout.
- **With KR-25:** one `Window`. `Surface.create(instance:, window: const ref Window)`, with the seam keeping a C ref on
  the window.

The commits:

0. (Shipped `0.9.634`: a bound plain enum is a `match` subject.)
1. The native seam: Window, Event, Key, headless, `needsWindow`, and the manifest entry.
2. The web html5 glue, resize/DPR, and pointer lock.
3. Gamepad.
4. The triangle quits on Escape and reconfigures on resize; docs.

CI with no display:

- `Window.headless(width:, height:)` plus `Window.post(event:)` (after SDL_PushEvent), so fixtures can test the queue,
  ordering and coalescing on the native, san and wasm-under-node legs.
- The key tables are tested by externing the C translators directly, as `tests/support/procutil.kama` does with
  kama_os.h.
- A build-and-link-only guard for the GLFW glue. CI installs no GLFW today, and GLFW 3.4's null platform is likely
  missing from Ubuntu 24.04's apt GLFW (3.3) — unverified.

Traps:

- On macOS GLFW must run on the main thread.
- `glfwTerminate` destroys every window, so init and terminate are ref-counted.
- On the web, pointer lock is granted only inside a user gesture, so it arrives later as a `PointerLock` event.
- `CloseRequested` never arrives on the web.
- Under node the headless window must register no html5 callbacks.
- Writing the translators: `match` on an integer is refused (use if-chains or `try cast`), `out` is a keyword, a
  statement `match` needs its `;`, and `case None: { break; }` needs its braces.

Size **L** (commit 0 is S, then 4).

**Decided 2026-10-16** (the maintainer took all five; the module became a package afterwards):

1. The name is **`window`** (`@kama/window`), holding `Window`, `Event` and `Key`: the window is where events come
   from and what a surface targets, and a program without WebGPU still gets input.
2. **A public `Window.headless` plus `post`,** which also serves replay and dedicated servers (SDL's public
   `SDL_PushEvent`).
3. **The physical key (USB HID ids), plus a separate `Text` event** — the DOM's `code` versus its `input` event.
4. **An explicit `pump()`.** An implicit pump inside `pollEvent` can livelock under a high-rate mouse.
5. **The coalescing ring plus `Event::Dropped(n)`,** never a silent drop.

<a id="s9"></a>

## 9. Performance

- **Float formatting is exact but not fast** (KR-23). Since `0.9.517` a float renders as its shortest round-trip
  decimal (SPEC *Formatting*), found by a search over the digit count with libc's `snprintf`/`sscanf` — checked
  against Python's `repr` (float64) and an exact rational reference (float32). It costs ~1.2 µs for a value that
  needs all 17 digits against ~0.33 µs for the old `%.17g`, and ~0.29 µs against ~0.19 µs for a short decimal
  (measured on the M-series dev box, 400k values each). A Ryu-class algorithm is faster than either and stays
  exact; it is the answer if a serialization benchmark (the KR-23 track) shows float output mattering.

Where kama currently stands is measured in [benchmarks/RESULTS.md](benchmarks/RESULTS.md) — read it there
rather than here, so there is one number to keep current. Forward work:

- **One `import` taxes every TU — SHIPPED `0.9.408`, and its win is UNCONFIRMED.** Split out of KR-68 on 2026-09-18, because that row was two
  problems wearing one sentence and only the expensive one had a plan. `emitIncludes`
  (`src/kama.cemit.cpp:34658`) walks every unit and UNIONS their `extern "<h>"` lines into a single
  `<name>.gen.h` that every generated `.c` includes. So the cost is not "the OS seam is heavy" but "one file's
  import is charged to every TU in the program".
  **Measured 2026-09-18, macOS** — a 7-file program with exactly ONE file calling `std::fs::exists` produces
  **17 TUs** (the stdlib contributes 11), and **all 17 include the seam**, `mm__m1.c` (`return x * 1`)
  included. Preprocessing that trivial TU: **22.51 ms with `kama_os.h` in the generated header, 18.30 ms
  without — 4.21 ms of pure waste, x16 innocent TUs = 67 ms per build**, on the platform where the seam is
  CHEAPEST (4,269 macros; Windows sees 21,748). It scales with module count, which is the direction real
  programs grow.
  **Why it was the better half to do first.** It touches no header, so it costs no inlining (the performance
  invariant is untouched), needs no change to the `--no-heap` header scan, and cannot fail open. It is pure
  emitter work against information the emitter already has per unit. And it may retire the depth half below: once the
  innocent TUs stop including the seam, the remaining depth win applies to the one TU that genuinely uses it,
  which may not clear the bar of trading away syscall-wrapper inlining. GOALS #2 (a TU that preprocesses less
  parallelizes better) and #4 / ponytail (a header carrying what a TU NEEDS is simpler than one carrying
  everything the program needs, and far less code than restructuring twelve headers).
  ⚠️ **The union is not arbitrary, and this is the row's real work.** A shared header guarantees every TU can
  see every extern TYPE, so a naive per-unit split breaks a `type extern value` whose layout crosses units.
  The set has to be DERIVED from the C each unit actually emits — the same "derive, don't declare" move the
  no-heap verdict made at `0.9.401` — rather than read off the `extern "<h>"` lines written in that file.
  ✅ **As built (`0.9.408`), and what it actually bought.** The union is gone: a header reaches the shared
  `<name>.gen.h` only when an extern TYPE, an extern ENUM or an `extern const` can put it in a shared
  declaration; everything else is derived per TU from the text that TU emits, against the extern names kama
  declares PLUS every `kama_`/`KAMA_` name the shipped headers define (the emitter writes references of its
  own — a `spawn` trampoline names `kama_isolate_t`, which no kama declaration mentions).
  ⚠️ **MEASURED, AND THE PREMISE DID NOT HOLD ON macOS: 0.32 ms per innocent TU** (interleaved A/B, min of
  40) — about 5 ms on a 350 ms build. The preprocessed output shrinks by only 164 lines, because
  `kama_runtime.h`, which every TU needs anyway, already pulls most of what `kama_os.h` needs here; the seam
  adds 3,157 macros and recording macros is nearly free. **The earlier 4.21 ms estimate this row was filed on
  was drift**, measured one arm after the other on a box whose run-to-run spread is ±13%. Interleave the arms.
  ✅ **AND ON WINDOWS IT DOES HOLD — measured 2026-09-20, so the change STAYS.** Same program, same method
  (interleaved A/B, min of 40), on the Windows VM: an innocent TU preprocesses in **132 ms with the fix and
  237 ms without — 104.5 ms saved per TU, 1.79x**, on a 17-TU program where 16 TUs are innocent. Beside the
  timing, the deterministic half, which no VM noise can touch: **7,029 preprocessed lines / 855 macros with,
  65,652 / 21,749 without.** macOS differed by 164 lines; Windows differs by 58,623. So the union really was
  charging every TU the whole of `<windows.h>`, and macOS's 0.32 ms was the platform where the seam is
  already-included rather than a verdict on the change. ⚠️ **The absolute times are QEMU-inflated** (that box
  emulates x64, and 132 ms to preprocess a trivial TU is not a Windows fact) — the RATIO and the line counts
  are the finding, which is also why the deterministic pair is recorded beside the clock.
  ⚠️ **The ordering contract it introduces, every line of it measured rather than reasoned about.** Each
  module TU must emit, in this order: the feature macros (`KAMA_ONPANIC`, `KAMA_GLOBAL_ALLOCATOR`), then
  `kama_runtime.h`, then its own FFI headers, then the shared header. Getting it wrong is not a compile
  error in two of the three cases — a runtime header pulled in ahead of the feature macros made
  `global_allocator_isolates` HANG, because the TU built the default malloc funnel for a program that had
  declared a pool. `kama_log.h` is not self-contained (it uses `size_t`), which is why `kama_runtime.h`
  precedes the FFI headers; and the FFI headers precede the shared header because they pull system headers
  whose functions the shared header's emitted bodies hand-declare ("cannot apply asm label to function after
  its first use").
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
  - **Modular / opt-in stdlib — ANSWERED 2026-09-16 at `0.9.359`: per-module opt-in is a NON-GOAL** (maintainer
    ruling on the measurement below; the row is deleted from ROADMAP.md, and the stdlib is cleared to grow).
    The question was whether emit-on-instantiation + `--gc-sections` suffices, or explicit per-module opt-in is
    warranted before the stdlib grows. The answer splits in two, and only one half was ever a packaging question.

    A ladder of release builds, native. To rebuild it: five programs from an empty `main` up to one
    importing fourteen `std::` modules, each built `--release --keep-c` **into its own directory** (a shared
    one makes every rung count its predecessors' C as its own — that mistake produced a plausible wrong
    answer first), counting C function definitions in the kept `.c` and taking `analyze` as the median of
    five `KAMA_TIMING=1 kama check` runs:

    | program | imports | closure units | analyze | fns emitted | binary |
    |---|---|---|---|---|---|
    | `fn int32 main() { return 0; }` | 0 | 2 | 9.0 ms | 268 | 16,944 B |
    | + `println` (prelude, no import) | 0 | 2 | 8.5 ms | 268 | 33,520 B |
    | + `DynamicArray`, used | 1 | 18 | 17.1 ms | 388 | 33,512 B |
    | 14 stdlib imports, **one used** | 14 | 35 | **302 ms** | 855 | **33,512 B** |
    | the same 14, **all used** | 14 | 35 | 320 ms | 1,209 | 34,368 B |

    **SIZE: pruning holds completely, so opt-in packaging buys nothing.** Importing fourteen modules and using
    one produces a binary *byte-identical* to importing none (33,512 vs 33,520 — noise), because the emitter's
    855 functions are compiled and then dropped by `-dead_strip`/`--gc-sections`. Actually using all fourteen
    costs +856 B, which is the real work. No per-module opt-in switch could improve on "zero".

    **TIME: the cost is the IMPORT, not the use** — +294 ms for imports a program does not use, and only
    +18 ms (6 %) for using them all. Measured per import, each costs 10–79 ms on its own
    (`SortedSet` 79, `PriorityQueue` 33, `path::join` 11) and they are roughly ADDITIVE even though the unit
    sets overlap heavily (18 units each, 35 in union): the cost tracks declarations analyzed, not files opened.

    **Growth is safe, which is what the row actually asked.** A program that imports nothing analyzes 2 units
    in 8.5 ms no matter how large `lib/std` becomes — closure pruning (shipped, §9) means a new stdlib module
    costs every non-importer exactly nothing, and nothing new goes in the prelude. So the stdlib may grow.

    **What is left is not a packaging question** — it is the build-speed one this section owns, which is why
    the verdict was moved here when the row was deleted and §3 (*open design questions*) retired with it.
    `analyze` dominates, and the lever is to emit only what is REACHED; see the bullet below.
  - **The floor is ANALYSIS, and it is paid per IMPORT rather than per use — measured 2026-09-16 at
    `0.9.359`** (the ladder and the table are in the modular-stdlib verdict just above). A program importing
    fourteen stdlib modules and calling one function from one of them analyzes 35 closure units in **302 ms**
    against a 8.5 ms floor, emits 855 C functions for the one it calls, and links a binary byte-identical to
    importing nothing. Using all fourteen adds 18 ms. So the lever is **emit (and analyze) only what is
    REACHED**, not a smaller stdlib or a packaging switch — and the reach-based call graph shipped for
    `--no-heap` at `0.9.348` is the same analysis pointed at a different question. Unrowed deliberately: it is one lever
    on the build-speed question this section already owns, and it wants its own measurement of where the
    302 ms goes (collect vs emit) before anyone designs it.
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
     `type adapter <int32> implements Parseable` registers a conformance for a *primitive* under no name.
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

### Toward the fastest server runtime (KR-254, KR-255)

**Where it stands (2026-10-28, kama 0.9.753, [benchmarks/servers.md](benchmarks/servers.md)).** With the server
saturated on 3 cores, kama's fibers (an isolate per core, a fiber per connection) are level with tokio's thread-per-core
runtime on echo and like-for-like HTTP at 100 and 1,000 connections and 1.1–2.9% behind it at 10,000; ahead of a
hand-written C epoll loop at 100 and 1,000 connections (4.0% and 0.8%); 1.2–1.3 times Go and 1.4–1.5 times Node's
`cluster`; and ahead of tokio's work-stealing runtime except at 10,000 connections, where it serves 6.5–7.9% more by
leaving some connections waiting (three times kama's p99). Thread-per-core loses where a few connections carry CPU
bursts at low load (KR-255). Compute is at C's speed already ([benchmarks/RESULTS.md](benchmarks/RESULTS.md)).
On x86-64 (an i9-13900K, kama 0.9.755, servers.md's *x86-64* section), kama is level with tokio's thread-per-core arm
from 1,000 connections and with its work-stealing runtime at 10,000, and the C epoll loop leads both kama and
thread-per-core tokio by 6–11% from 1,000 connections.

The first run, on 2026-10-27, mostly measured a trap in the bench VM: podman ran it nested, so every kernel clock read
cost about 1 µs (servers.md says how to check). On the fixed VM, the 1.1% and 4.8% it showed against tokio's
thread-per-core arm were really 7.4% and 5.6%. kama 0.9.752 closed most of that by skipping the `recv` that can only
answer `EAGAIN` after a short read (tokio's rule; [design/fibers.md](design/fibers.md) decision 7), and 0.9.753 by
prefetching the next fiber's saved frame. Asked on 2026-10-27 "what can we do to become the most performant overall?",
the maintainer chose this order: measure what the benchmark leaves out and close the one measured gap first, then
cross-module inlining, then an io_uring design. Cross-module inlining was in place for native already (a `--release`
build folds the program into one translation unit, which beat ThinLTO 3.5 times on `math`); wasm gained the same fold
in `0.9.755`, where `math` ran 2.46 times faster for it ([SPEC.md](SPEC.md) *Building & debugging*). The io_uring
arm, the third, shipped in `0.9.756`–`0.9.765`: on x86-64 1.08–1.24 times epoll's requests at 0.81–0.92 of its CPU,
and on the M4 level up to 1,000 connections and 5–6% less CPU at 10,000 (servers.md's *io_uring* section).

**The field beyond the run.** The fastest servers today are io_uring thread-per-core runtimes (Seastar in C++,
glommio and monoio in Rust), which complete I/O with fewer system calls than the epoll kama and tokio both use, and
hand-tuned HTTP stacks (vectorized parsing, pipelining) such as Go's fasthttp, .NET's Kestrel and the C++ and Rust
servers at the top of TechEmpower's plaintext test. Java's virtual threads are kama's model on the JVM and were not
measured.

<a id="s9-ring-large-chunks"></a>

### The ring arm behind epoll at large chunks (KR-317)

Found 2026-11-04 by the measurement `split` shipped behind. Full-duplex TCP over loopback on one thread, a reader and a writer each side
(`bench/pipe/`, `split`; nanoseconds a chunk each way, the minimum over 7 interleaved rounds, a Linux VM on an M4, the ring allowed):

| | kama, io_uring | kama, epoll | tokio | Go |
|---|---|---|---|---|
| 1 KiB chunks | 1,128 | 1,123 | 1,161 | 2,822 |
| 64 KiB chunks | 13,107 | 12,361 | 12,403 | 15,061 |

Paired by round, epoll is 9% ahead of the ring at 64 KiB in all 7 rounds, and at 1 KiB they tie. kama on epoll ties tokio, so a program on
kama's default arm is what falls behind. It is not the split, which is one pointer load before a system call. Not yet known: where the
time goes (a `RECV` that returns short, more enters a chunk), and whether x86-64, where every write goes through the ring, shows the same.
The ring was chosen on server workloads, whose reads are small; KR-254's ruling on writes is a per-platform rule of this kind.

<a id="s9-fiber-memory"></a>


#### Memory per idle fiber connection (KR-254)

An idle connection on the fiber arm holds 8.4 KiB (process baseline included) against Go's 8.2, Node's 8.5, tokio's 5.0
and the C loop's 4.2. Most of the difference is the fiber's touched stack, at least a page, which a stackless task
does not have; that floor is the price of fibers without colouring (fibers.md decision 1). Measure what each connection
touches — the stack, the fiber's control block, the socket's registration, the arm's 4 KiB buffer — and trim what is not
the floor: pooled stacks and buffers, a smaller first frame.

The same footprint is what is left between kama's fibers and tokio's thread-per-core arm at 10,000 echo connections.
Switching into a fiber loads its 160 bytes of saved registers from a stack that went cold while thousands of others
ran: about 3.5% of the server's samples at 1,000 connections, in `kama__fiber_swap` (perf files them under the symbol
before it, `frame_dummy`, since the asm label has no function type). `0.9.753` prefetches the next ready fiber's frame
while the current one runs: 1.7% less CPU per request at 10,000 connections (15/15 rounds), and no change at 1,000,
where the frames stay cached. After it, kama and tokio-percore spend the same CPU per request at 10,000 connections,
and tokio serves about 1% more requests a second; at 1,000 the two are within the noise floor. Frames and control
blocks laid out densely, so that a switch touches fewer lines and pages, are the lever left. On x86-64 the two are level
at 10,000 (servers.md's *x86-64* section). Hardware counters (cycles, cache and dTLB misses for the fiber arm against
tokio-percore, `perf stat` through the harness's `WRAP`) are still owed: neither VM exposes a `cpu` PMU, so they wait
for a machine that passes one through.

**The io_uring arm left a decision here (2026-10-29).** On io_uring, `kama_net_write`'s rule is gated by
architecture, as a measured delta:
- x86-64 hands every write to the kernel as a `SEND`;
- elsewhere a write makes its system call first, and is a `SEND` only when the socket is full.
Both rules on both machines are servers.md's *io_uring* table. Every write a `SEND` wins on x86-64: 1.22× and 1.08×
epoll's requests at 1,000 and 10,000 connections, against 1.09× and 1.00×. On the M4 neither wins everywhere. Every
write a `SEND` is level or better up to 1,000 connections and costs 8% more CPU at 10,000; the system call first is
the reverse, 1–2% more CPU per request than epoll up to 1,000 and 5–6% less at 10,000 (the closing check, `0.9.765`,
15 rounds, [benchmarks/ring-close-aarch64.tsv](benchmarks/ring-close-aarch64.tsv)). The M4 ships with that cost at low
density (ruled 2026-10-29), and this row owns it. What separates the rules is the second park a `SEND` costs per
request, a switch back into a fiber whose stack has gone cold, about 0.3 µs at 10,000 connections against a 0.25 µs
`send`. Once this row cuts that cost, measure both write rules on both machines. If every write a `SEND` wins on both,
delete the gate: one rule, x86-64's, and the M4's low-density cost goes with it. The x86-64 box runs its half.

**Measured again on the M4, 2026-10-29 (`0.9.763`, the io_uring arm's profile):** at 10,000 echo connections the
switch is 5.3% of the epoll arm's server CPU, about 0.18 µs each time a fiber is switched into, and the first lines its
code then touches on its cold stack cost about 0.1 µs more. That is more than the ~1% against tokio suggests, because
it is paid on every park, and it decided the io_uring write path: handing every write to io_uring meant a second switch per
request, which on the M4 cost more than the `send` system call it saved, so a write now makes its system call first
(`0.9.764`). A cheaper switch into a cold fiber would help every fiber program, and could let the ring take every
write again, which x86-64 preferred in the io_uring design's C harness ([benchmarks/uring-x86-64.tsv](benchmarks/uring-x86-64.tsv)). Both arms' flat profiles are in
[benchmarks/ring-profile-aarch64.txt](benchmarks/ring-profile-aarch64.txt).

**The cold switch: the plan (2026-10-29), which may let the write gate go.** What a switch into a fiber touches today
(`include/kama_fiber.h`):
- **Its saved registers, on its own stack:** 160 bytes on aarch64 (x19–x30 and d8–d15), three cache lines; on x86-64
  the callee-saved set is about one line. That difference may be part of why x86-64 tolerates a park that the M4 does
  not.
- **A stack per fiber, each its own 256 KiB `mmap` with a guard page below it.** So 10,000 fibers put their saved
  contexts on 10,000 different pages, which no TLB covers (and a guest's miss walks two levels of page tables), and
  every context sits at the same offset within its page, so they compete for the same few cache sets.
- **The control block, from `kama_alloc`**, which the scheduler has just touched to pop the fiber from its queue.

The candidates, each cheap to try once the first step exists:
1. **A cold-switch microbenchmark first** (`bench/fiber/`, beside `bench/poller/`): N fibers parked and woken round
   robin with no I/O, nanoseconds per switch-in at N = 100, 1,000, 10,000 and 100,000, on both machines. It isolates
   the cost the server benchmark buries, and judges every candidate below in seconds rather than a 40-minute run.
2. **Prefetch on wake:** when a completion or an event makes a fiber ready, prefetch its saved context then. It runs a
   whole ready queue later (up to 61 fibers), long enough to hide a miss and a page walk, where `0.9.753`'s prefetch of
   the next fiber only has one run's time.
3. **The saved context in the control block,** as Go keeps a goroutine's registers in its `g`: the switch loads from
   memory the scheduler touched a moment before, and no longer from the stack's page. It changes the switch on every
   architecture (aarch64, x86-64, Windows x64), so it is measured against 2 and 4 before it is chosen.
4. **Stack colouring:** start each fiber's stack at a different offset within its page, so the contexts stop sharing
   cache sets. It costs at most a page of a 256 KiB stack.
5. **Stacks from one arena,** guard pages kept, so neighbouring stacks share page-table pages and a walk is shorter.
   Huge pages are not an option while each stack has its own guard page.

**Measured 2026-10-29: candidate 1 built, 2 and 4 prototyped** (`bench/fiber/`, 10 interleaved rounds each; raw rows
[benchmarks/coldswitch-linux.tsv](benchmarks/coldswitch-linux.tsv) and
[coldswitch-macos.tsv](benchmarks/coldswitch-macos.tsv)). N fibers in a ring pass K tokens, so each hop is one park
and one switch into a fiber that last ran N hops ago; K = 64 keeps the ready queue as deep as the server's turn. A
`touch` arm makes the same channel calls from one fiber with no switch: 15–37 ns a hop at every N, so the growth below
is the switch, not the channels. Each prototype against the tree's own build, paired by round (`ring#2` sits within 1%,
3.5% at one config):

| Linux VM (the server's) | hop | colour | prefetch on wake | both |
|---|---|---|---|---|
| 100, K = 1 | 93 ns | 0.77 · 10/10 | 0.92 · 9/10 | 0.78 · 10/10 |
| 10,000, K = 1 | 227 ns | **0.57** · 10/10 | 0.90 · 9/10 | **0.55** · 10/10 |
| 10,000, K = 64 | 188 ns | **0.88** · 10/10 | 0.99 | 0.90 · 10/10 |
| 100,000, K = 1 | 388 ns | 1.05 · 1/10 | **0.93** · 9/10 | 0.99 |
| 100,000, K = 64 | 371 ns | 1.04 · 3/10 | 0.97 · 8/10 | 1.035 · 2/10 |

macOS (16 KiB pages) agrees: colour 0.69 at 10,000 with K = 1 (421 → 290 ns), 0.94 with K = 64, and 1.03 at 100,000.

What it says:
- **The cost is cache-set conflict, as the plan suspected.** Every stack's top is page-aligned and every fiber parks at
  the same depth, so every saved context sits at the same offset in its page, and the L1, indexed by those bits, holds
  only a few dozen of them however many fibers there are. Starting each stack's top one cache line further into its
  page than the last (the prototype: `colour++ % (page / 64)` lines, `kama_fiber_new`) takes 43% off a hop at 10,000
  fibers, and 23% even at 100, where 100 contexts already overflow three sets.
- **Prefetch on wake is small, and adds nothing once the contexts are coloured.** Alone it is 7–10% at 10,000 and
  100,000 with K = 1.
- **Colour costs 4–5% at 100,000 fibers.** The likely cause: a top shifted up to a page lets the frames under it cross
  into the page below, so a switch-in touches two pages where it touched one, and at 100,000 every page walk misses.
  A colour range that keeps the deepest park frame inside the top page would test it.

**The range, measured 2026-10-29** (Linux VM, 10 rounds, [benchmarks/coldswitch-range-linux.tsv](benchmarks/coldswitch-range-linux.tsv)),
each against the tree's own build: the whole page 0.774 at 100 fibers, 0.583 and 0.876 at 10,000 (K = 1, 64), 1.049
and 1.054 at 100,000; half a page 0.904, 0.586, 0.895, 1.029, 1.052; a quarter 0.987, 0.697, 0.884, 1.016, 1.033. **The
whole page**, then: a bound gives up gains and keeps the cost at 100,000, so the two-pages explanation is wrong. A
likelier one: uncoloured, 100,000 thrashing contexts stay in a few sets; coloured, they sweep the whole cache and evict
the scheduler's and the channels' lines too. The server at 10,000 connections on 3 cores runs about 3,300 fibers a
core, where colour wins; the 4–5% at 100,000 a core is recorded as measured. Prefetch on wake beside colour recovers
about 6% at 100,000 with K = 1 and nothing with K = 64, the server's depth, so it is not kept; `0.9.753`'s prefetch of
the next ready fiber stays.

**Built (`0.9.767`), and the server does not show it** (the M4, 15 rounds, both kama arms coloured,
[benchmarks/colour-rules-aarch64.tsv](benchmarks/colour-rules-aarch64.tsv)). CPU per request against the epoll arm,
the system call first / every write a `SEND`: echo 1.023 / 1.008 at 100, 1.011 / 0.987 at 1,000, 0.963 / 1.084 at
10,000; HTTP 1.018 / 1.005, 1.014 / 0.993, 0.943 / 1.066. The rules split as they did uncoloured, so the gate stays, and
against the C loop in the same rounds kama's CPU per request is where it was (0.93 of C's at echo 10,000, 0.92 before).
In the microbenchmark a fiber touches little but its stack, so set conflicts dominated; a server's fiber also touches
its socket, its buffers, its control block and the kernel's state for its connection, some 3,300 of each per core, and
those misses are capacity, which colour cannot move. **Ruled 2026-10-30, as recommended:** colouring stays, and the
write rule's split is a permanent measured delta (SPEC's *Sockets on a fiber*), re-measured only if the footprint work
below makes the second park cheap on the M4 (one 90-minute run: `kama-fibers-b` from a worktree whose gate reads
`#if 0`). What this row does next is what it began as: shrink what a connection touches (its control block, its
registration, its buffers), the capacity the server's misses are made of. The question as it was put: keep colouring (12–43% where a fiber's
working set is small, as in channel pipelines; 4–5% worse at 100,000 fibers a core; neutral in the server) or revert
it; and whether the M4's 10,000-connection park cost is worked further (candidate 3 moves the context load off the
stack page, but the fiber's frames are on that page, so the page walk would only move), or the write rule stays a
permanent measured delta and this part of the row closes.

**Ruled 2026-10-29: as recommended** — build stack colouring, the range chosen by measurement first; prefetch on wake only if it still pays at 100,000 beside it; 3 and 5 only if the server still shows the cost. The decisions as they were put: build stack colouring, and with what range (the whole page, or only what keeps a park's
frames in one page, measured against each other first); whether prefetch on wake is kept beside it (only if it still
pays at 100,000 on top of the chosen colour); and 3 and 5 are not built unless the server at 10,000 still shows the cost
after colouring.

**Done when:** the switch into a cold fiber is cut, the server's echo 10,000 shows it, and then **both write rules are
measured on both machines** (the quick run, then the closing check). If every write a `SEND` wins on both, the gate in
`kama_net_write` goes and x86-64's rule is the one rule; if the system call first wins on both, the gate goes the other
way; if they still split, the gate stays as a measured delta. Decisions owed before code: which candidates to build
after the microbenchmark ranks them, and, if 3 is chosen, the context's layout per architecture.

<a id="s9-skew"></a>

#### Thread-per-core under a few connections with CPU bursts (KR-255)

**Measured** (servers.md, *Skew at low load*; 15 rounds each). With 12 connections, 2 of them asking for a CPU burst
on every request, tokio's work-stealing runtime serves 1.29 times kama's requests with 100 µs bursts and 1.91 times with
1 ms bursts (15/15 each), at a light-connection p99 of 131 and 123 µs against kama's 221 and 1,245. Go serves 1.12 and
1.59 times. tokio's thread-per-core arm and the C loop fare as kama does. From 100 connections up, kama is ahead of the
work-stealing runtime (12% at 100 connections, 9% with 1 ms bursts at 1,000, at a third of its p99).

**Why.** `SO_REUSEPORT` hashes each connection to a core when it connects, and knows nothing about what it will cost.
A core holding a hot connection serves its others only between bursts, while another core sits partly idle (`busy`
80–88%). A work-stealing runtime moves the waiting tasks to the idle core. With 100 connections or more every core's
queue is full, so stealing has nothing to do and its unfairness costs it.

**Placement alone costs nothing; the burst is the case.** Plain echo with no bursts, at 12 and 24 connections (15
rounds, 2026-10-28; servers.md): kama is level with Go and 3.3% ahead of work-stealing tokio at 12 (15/15), and 1.26
times Go and 1.40 times tokio at 24 (15/15). The kernel's uneven spread varies kama's runs (465,000 to 601,000 requests
a second at 24 connections), but its slowest run there still beats tokio's fastest. Thread-per-core loses only when a
burst holds a core.

**What other runtimes do**, about a burst inside a request and about where a connection runs:

| runtime | a CPU burst in a request | where a connection runs |
|---|---|---|
| Go | preempted after 10 ms; other Ps steal the goroutines waiting behind it | any P, stolen |
| tokio (multi-thread) | `spawn_blocking` or rayon by convention; otherwise other workers steal the queued tasks | any worker, stolen |
| Java virtual threads | runs on its carrier; the ForkJoinPool steals the others | any carrier, stolen |
| Seastar | stays on its shard; a task quota makes long work yield, and heavy work is split or sent elsewhere explicitly | its shard for good (Scylla's drivers are shard-aware) |
| glommio, monoio | stays on its executor; glommio weighs executors by shares | its core for good |
| OCaml 5 Eio | `Executor_pool` | its domain |
| Envoy | stays on its worker | `SO_REUSEPORT`, or "exact balance": each new connection to the worker holding fewest |

**The production answer is decision 5's job pool (KR-3 step 9), already ruled on 2026-10-22.** Fibers never migrate:
they hold state that is not `Sendable`, so moving one between isolates would break kama's sharing rules. A fiber with a
burst submits it as a `Sendable` job and parks on its result. Its core serves its other connections meanwhile, and the
pool's workers steal among themselves, so the burst lands on whichever core is free. That is what Seastar, glommio and
tokio programs do (`spawn_blocking`, rayon), and the program names its burst where it occurs. In the shape decision 9
gives a fiber (step 9 rules the job pool's own spelling):

```kama fragment
// today: the burst runs on this fiber's core, and every other connection there waits for it
uint32 h = mix(b: buf, rounds: rounds);

// with step 9: the burst runs on the pool, this fiber parks, and its core keeps serving
Job<uint32> j = spawn job mixJob(work: give MixWork.of(bytes: buf.slice(from: 0, count: 64), rounds: rounds));
uint32 h = join(job: give j);
```

What it costs, per burst: the job's bundle copied to a pool thread (64 bytes and a count here), one submission, one
park and one completion, a few microseconds against a burst of 100 µs to 1 ms. A process can run up to twice as many
threads as cores, which is decision 5's stated cost.

**Checked against GOALS.md:** the pool is a `std::concurrent` library in kama (#1); a job's bundle is a visible,
`Sendable` allocation (#3); no state that is not `Sendable` crosses an isolate, because fibers stay put (#3a); it is the
one way to move task-parallel CPU work, beside `parallel_for` for data-parallel work (#4); and the burst is named at its
call (#5). It is consistent with practice: it is decision 5 as ruled, and decision 5 named this case as its accepted
cost ("uneven long-lived connections stay where they were accepted"). This row measures that cost and confirms the
remedy.

**Ruled 2026-10-28, as recommended:**
1. The job pool, as decision 5 rules. When step 9 lands, rerun `skew:12` with the burst offloaded, and close this row
   on that result.
2. Load-aware placement (an eBPF `SO_REUSEPORT` program, or Envoy's exact balance) is not scheduled: it is measured
   unnecessary above, and it could not have helped the burst case, since a connection's cost is unknown when it
   connects. It reopens only on a measurement where connections of even cost, unevenly placed, cost thread-per-core
   against stealing.

<a id="s9-tu-header-cost"></a>

### What a user TU parses (KR-68)

**What is open.** Every user TU includes the program's `gen.h`, which carries the prelude's `static inline` bodies and
every generic instance, and `kama_runtime.h`, much of which is macro-generated bodies a given TU never calls. In a
`std::fs` program `main.gen.h` is 9,431 lines against `kama_runtime.h`'s 1,291 expanded. Measured 2026-09-18 on
macOS: a TU with the OS seam removed still paid 18.30 ms of its 22.51 ms to `kama_runtime.h` — though about 13 ms of
any TU is clang starting up (2026-10-15). Nobody has measured what a TU actually USES of either, and that measurement
comes before any proposal. Size `?`.

**Settled, and why the row was re-pointed (decided 2026-10-16, shipped `0.9.647`).** The row began as "move the OS
seam's ~158 `static inline` bodies into one implementation TU, so `kama_os.h` stops pulling 25 system headers into
every TU". Two later facts retired it:

- `0.9.408` gives each TU only the headers its own text names, so on macOS only the stdlib TUs that call the
  seam include it (4 of 14 in a `std::fs` program, `main.c` not among them), and the per-unit cache skips them after
  the first build. The seam's depth is a clean-build cost only.
- Measured interleaved, min of 30, clang on macOS: preprocessing an empty file / `+ kama_runtime.h` / `+ kama_os.h`
  costs 12.9 / 17.4 / 22.6 ms, and parsing 13.3 / 23.3 / 33.1 ms. That is about 10 ms per seam TU, about 40 ms per clean
  build. **An implementation TU compiling all the bodies at -O2 costs 105.7 ms against 33.2**, so the split is a net
  loss.

What shipped instead are the Windows trims. `kama_os.h`, `kama_time.h` and `kama_random.h` already defined
`WIN32_LEAN_AND_MEAN`, which only stops `<windows.h>` pulling sub-headers a user can still include. They now also
define `NOGDI`, `NOUSER` and `NOMINMAX`, which remove declarations and macros for the rest of the TU — and
`<windows.h>` is include-guarded, so a user's own C header after them could never get `CreateWindowExW` or `min`
back. A native release build folds every module into one TU behind `gen.h`'s includes, so the trims are gated per
PROGRAM: the compiler writes `KAMA_OWN_HEADERS_ONLY` ahead of each TU's headers only when every header any unit names
is one kama ships or one of C's own (emitIncludes). Measured with zig's mingw-w64 headers (the toolchain's), a
`std::net` TU preprocesses to 77,037 lines against 81,765 and parses in 200.6 ms against 204.0 (min of 15,
interleaved): about 3 ms per seam TU. A program with any C header of its own keeps the full `<windows.h>`.
`tools/check-header-scan.sh` stays: the no-heap verdict reads these bodies where they are.

<a id="s10"></a>

## 10. Tooling / distribution (deferred)

<a id="s10-generate-step"></a>

### A manifest step that generates code before analysis (KR-338)

A game engine on kama generates part of each game's code from records, and decided not to commit the output. A game's
own sources import the generated module (`import { game::gen::World }`), so until something has written it every
front door fails. **Measured `0.9.860`:** a package listing `gen` under `modules`, with `src/gen` absent as git leaves it on a
fresh clone, fails `kama check kama.json` with "cannot resolve module 'game::gen'"; restored, it checks. (The note that
used to blame the manifest now names the missing folder, `0.9.864`.) None of the keys `docs/packages.md` lists runs a
step first.

**What others do.** Cargo runs a package's `build.rs` before compiling it, and the build script writes into `OUT_DIR`,
re-run when the files it declares with `rerun-if-changed` change. Zig's `build.zig` is a program that can add a
generate step whose output a module imports. Bazel and Buck have genrules with declared inputs and outputs. Go's `go
generate` runs only by hand, and its output is committed, which is the other answer: no step, and the repository holds
generated code.

**The shape the report asks for:** a manifest entry naming a step that kama runs before analysis, with its inputs and its
output module declared, so kama can skip it when nothing changed and `kama query` and the language server see what it
wrote. The step is a kama program (a workspace member under `tools/`, say), so it needs no shell and runs the same on
every host. Open for the design: whether the output lands in the source tree or under `out/`, how `kama lsp` runs a step
it must not run on every keystroke, what a step may read (only declared inputs, as Bazel enforces, or anything, as
`build.rs` may), and whether a dependency's step runs in its consumer's build. **Not a non-goal:** committing generated
output works today, but it leaves a generator's users to remember to run it and to review machine-written diffs, and a
build step is how the languages kama measures itself against answer it. Wanted by the owner as a general-purpose tool.

<a id="s10-release-signing"></a>

### Signed releases (KR-135)

`release.yml` signs nothing on any platform. **Scheduled** for when releases go to people outside this repo.

**Windows — measured 2026-10-04 on a Windows 11 Home box.** Smart App Control left evaluation mode on its own
(`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy`: `VerifiedAndReputablePolicyState` 1, `SAC_PreviousState` 2)
and from then on refused to start msys2's `clang++.exe`, a source-built `kama.exe`, and every executable a kama
build produced: *An Application Control policy has blocked this file*, event 3077 in
`Microsoft-Windows-CodeIntegrity/Operational`. Two consequences, and the second is the one that bounds this row:

- A signed release `kama.exe` is what lets a SAC machine start kama at all. Microsoft describes SAC as allowing
  an app with a valid signature from a trusted CA when its cloud service has no verdict. **Unprobed here**: the
  first signed build is the probe.
- Signing kama does nothing for what kama BUILDS. Each output is a new unsigned executable, and SAC blocks it as
  it blocks the output of every compiler. Signing users' programs is not kama's job, so a development machine
  runs with SAC off. That is a `docs/platforms/windows.md` line, not part of this row.

Two ways to sign: Azure Trusted Signing (a Microsoft-held key used from CI, a monthly fee, identity validation
first), or an OV certificate, whose key has had to live on a hardware token or a cloud HSM since 2023, which is
awkward from a hosted runner. Check the price and eligibility when this is picked up. ⚠️ The bundled variant
also ships `zig.exe`. Whether zig's Windows release is signed is unprobed. If it is not, a bundled install on a
SAC machine starts kama and then cannot start its compiler.

**macOS.** The maintainer already holds a Developer ID Application certificate and a notary API key (since
2026-09-30), in a login keychain. CI needs the same identity exported as a `.p12` repository secret and imported
into a temporary keychain on the runner, plus the notary key as a second secret. Notarization wants every Mach-O
in the submission signed with that identity, under the hardened runtime (`--options runtime`) and with
`--timestamp`, so the bundled `zig` is re-signed with ours. A ticket cannot be stapled to a bare Mach-O (only
to an `.app`, a `.dmg` or a `.pkg`), so a tarball's binary is checked online at first launch. Shipping a `.pkg`
instead, so the ticket travels with it, is the open choice.

<a id="s10-agent-tooling"></a>

### kama's own AI-agent tooling (KR-148)

Asked for 2026-10-14: kama should carry its own tooling so that an LLM or an agent can work in a kama codebase with
the compiler and the language server behind it, beside the `llms.txt` it already ships. The ask names a skill (one
that would likely drive the language server), agentic workflow helpers embedded in this repo, the `ponytail` skill
(github.com/DietrichGebert/ponytail) as a reference for the shape, and using the same tooling to develop kama itself.

**Brief, 2026-10-15 on `0.9.626` — its decisions open the next session.** The hosts and peers below were read from
their own docs or source that day; the timings were measured on this Mac.

**What kama ships today.**
- `kama agents install` writes `AGENTS.md` (200 lines, embedded in the binary); a one-line pointer to it for each of
  six tools that read their own file first; `AGENTS.package.md` for a library; and, with `--skill`, the 129-line
  `kama query` cookbook at `.claude/skills/kama/SKILL.md`. Claude Code now reads `AGENTS.md` natively when a project
  has no `CLAUDE.md` (v2.1.277+, code.claude.com/docs/en/memory), so the `--claude` pointer is for older versions and for a
  project that already has a `CLAUDE.md`.
- `kama query` costs 0.03 s on one fixture and 0.21 s for `--diagnostics` on `lib/std/process/process.kama` with its
  closure, but **1.75 s for `--search` at package scope over the stdlib's 14.6k lines**. Package scope analyzes every
  file the manifest claims, so the cost grows with the project and is paid per process. Every mode except `--search`
  takes a `LINE:COL` (1-based, 0-based), and the skill already has to say "try one column left".
- `kama lsp` holds the same analysis resident and answers diagnostics, hover, definition, references,
  implementation, rename, document and workspace symbols, completion, signature help, semantic tokens and an
  auto-import fix. It has **no call hierarchy** — KR-33's first residual.
- `llms.txt`, at the root and on kama-lang.org.
- This repo's own sessions use none of it. `.claude/skills/` vendors `ponytail` and `karpathy-guidelines` only, no
  language server is in the agent's loop, and load-bearing procedures live only in an assistant's per-account memory
  — for one, *a grammar change is four artifacts plus a pins commit*, which no repo doc states whole. AGENTS.md's
  *Where things are written down* forbids exactly that.

**What the hosts offer** (code.claude.com/docs: plugins-reference, code-intelligence, skills, marketplace-reference).
- **A code-intelligence plugin.** A plugin's `.lsp.json` names a server (`command`, `args`, `extensionToLanguage`).
  Claude Code starts it the first time a matching file is edited, **pushes its diagnostics into the conversation
  after every edit** ("Found N new diagnostic issues"), and gives the model a read-only `LSP` tool: definition,
  references, hover, document symbols, workspace symbol search, implementations, call hierarchy. Anthropic maintains
  such plugins for 12 languages (gopls, rust-analyzer, clangd, pyright, sourcekit-lsp, …). The one language *vendor*
  in the official list is Shopify (Liquid), whose own repo is its marketplace. When a server's binary is on `PATH`
  and an added marketplace lists a plugin for the extension, Claude Code offers to install it.
- **Skills** follow the open Agent Skills standard (agentskills.io, 46 clients listed: Codex, Copilot, Cursor,
  Gemini CLI, Junie, Kiro, …), and `.agents/skills/` is the cross-agent install path. Claude Code adds two things: a
  line `` !`command` `` in a skill runs when the skill loads and is replaced by its output, and `paths:` loads a skill
  only for matching files.
- **A plugin's source can be a `command`**, "when a tool installed on the user's machine produces the plugin
  directory"; Claude Code re-runs it once per session.
- **Hooks** (PostToolUse on Edit|Write) and **MCP servers**, whose tool definitions are charged to every session,
  are the other two channels.

**What peers ship.**
- **Go: `gopls mcp`**, experimental since gopls v0.20 (July 2025), over stdio or attached to the editor's session.
  - Its default tools are `go_workspace`, `go_package_api`, `go_diagnostics`, `go_rename_symbol`,
    `go_symbol_references`, `go_search`, `go_file_context` and `go_vulncheck`.
  - Its source leaves the position-based `go_references` off by default: it "requires a location, which models tend
    to get wrong. The symbolic variant seems to be easier to get right".
  - MCP cannot push, so the instructions (`gopls mcp -instructions`) tell the model: "After every code modification,
    you MUST call the `go_diagnostics` tool".
  - The Go blog (2026-02) gives the reason: diagnostics as "guardrails" for models that deny newer APIs exist.
- **Dart/Flutter** ships the most:
  - `dart mcp-server`: experimental, about 24 tools (analyze, LSP lookups, run tests, pub search, format);
  - an official Claude plugin repo, `flutter/agent-plugins`, bundling skills with that server;
  - rules files in four sizes;
  - **skills shipped inside packages.** A pub package carries `skills/`, and `dart run skills@ get` installs its
    dependencies' skills into `.agents/skills/`.
- **Deno** has an agent entry point (`deno.com/agents.md`) and a skills repo whose CI type-checks every code block and
  checks every `deno <subcommand>` against the installed Deno.
- **Kotlin** has `kotlin-agent-skills`: task-shaped skills with Claude and Codex plugin manifests.
- **Rust** has nothing official. rust-analyzer's maintainers declined: "not the job of rust-analyzer to implement an
  interface for LLMs".
- **ponytail**, the shape the ask named:
  - one canonical `SKILL.md`;
  - an `AGENTS.md` that is a "compact always-on instruction set for agents without skill support";
  - thin per-host adapters ("Keep adapters thin.");
  - the repo as its own Claude marketplace;
  - a CI script that fails when a rule copy "drifted from AGENTS.md".

  That is the design `kama agents` already has: one text, pointers never copies, and `check-agents.sh` holding the
  embedded copy byte-identical.

**The gap, in order of what it costs an agent.**
1. **Feedback depends on the agent remembering to ask.** An agent learns an edit broke the program only if it runs
   `kama check`. Every peer with a language server now has that pushed after each edit. kama has the server and does
   not wire it in.
2. **The skill is a copy that goes stale.** `--skill` writes the text of the compiler of that day, so a later
   compiler's query modes never reach it, and only Claude Code reads it where it is written.
3. **Positions are what models get wrong**, by the gopls team's finding and by our own skill's warning, yet every
   `kama query` mode but `--search` takes one.
4. **This repo does not use its own tooling**, so nothing exercises it daily.

**Recommendation.** One plugin whose every byte comes from the installed compiler, with the language server kama
already has as its channel. The CLI stays unchanged as the interface for every other host.

1. **A `kama` plugin, four small files, nothing copied:**

   ```
   .claude-plugin/plugin.json    {"name": "kama", "description": "…", "license": "MIT OR Apache-2.0"}
   .lsp.json                     {"kama": {"command": "kama", "args": ["lsp"], "extensionToLanguage": {".kama": "kama"}}}
   skills/kama/SKILL.md          frontmatter, then one line:  !`kama agents print --skill --body`
   skills/kama-package/SKILL.md  frontmatter (paths: kama.json), then:  !`kama agents print --package --body`
   ```

   An agent that edits `src/app.kama` to `int8 a = big;` then sees, without running anything:
   `Found 1 new diagnostic issue … a local is declared int8, so it cannot be initialized with a int32 — …`.
   The skill text is printed by the `kama` on `PATH`, so it describes the compiler actually in use, and the plugin
   needs no release of its own. Where a policy turns off skill shell execution, the line becomes a placeholder; the
   stub's one line of prose then says to run `kama agents print --skill`.
2. **Hosted in this repo.** `.claude-plugin/marketplace.json` goes at the root and the plugin under
   `agents/claude-plugin/`, four static files that are not embedded. Users add it with
   `/plugin marketplace add cosmic-canopy/kama` (sparse checkout). Listing it in Anthropic's directory is the
   maintainer's act. That listing is what makes Claude Code offer the plugin, unasked, to anyone with `kama` on `PATH`.
3. **`kama agents install --skill` also writes `.agents/skills/kama/SKILL.md`**, the cross-agent path, and
   `kama agents list` says Claude Code reads `AGENTS.md` natively from v2.1.277.
4. **Symbol-named queries.** Every cursor mode also takes a symbol instead of a position:
   `kama query kama.json --refs app::util::encodeTo`. It resolves as `--search` resolves, and an ambiguous name is
   refused, naming the candidates. gopls made this its default, for the reason above.
5. **Call hierarchy, pulled forward from KR-33.** `kama lsp` gains `callHierarchy/incomingCalls` and
   `outgoingCalls`, and `kama query` gains `--callers` and `--callees`. The LSP tool exposes the hierarchy, and "who
   calls this" is the question an agent asks before a change.
6. **A guard on the guidance**, as Deno has. Every `kama …` command line in the skill and in `AGENTS.md` runs against
   a fixture project, and the plugin's JSON is checked against the documented schema (strict keys), with
   `claude plugin validate` where that CLI exists. It joins `check-agents.sh`.

**Not recommended, each with its reason.**
- **An MCP server (`kama mcp`).** Go and Dart both ship one, and that is the strongest case for it. Against it:
  - It is a second way to ask what `kama query` and `kama lsp` already answer, and its tool list is charged to every
    session.
  - It cannot push diagnostics either: gopls has to tell the model to call `go_diagnostics` after every edit, and
    AGENTS.md already says the same of `kama check`.
  - Its one real gain, a warm index for a host with neither shell nor LSP, is the query floor (§9). That fix helps
    every caller if it is made under `kama query`.

  Proposed verdict: a **non-goal**, answered by the CLI plus the server, and revisited the day a host matters that
  has no shell.
- **A PostToolUse hook running `kama check`.** The plugin's pushed diagnostics already do this, from a warm analysis
  rather than a cold one per edit.
- **A kama flavour of ponytail.** ponytail is how to write less code in any language, and a user who wants it can
  install it. kama's own idioms already live in `AGENTS.md` and *coming-from-other-languages*.

**This repo: developing kama with the same tools.**
- Enable the plugin for this repo's sessions in `.claude/settings.json`: `extraKnownMarketplaces` names the repo
  itself, and `enabledPlugins` turns it on.
- The server and the skill's `print` resolve to `out/<os>-<arch>/kama` through a launcher, which `tools/kama-bin.sh`
  already makes one line. Never the `./kama` symlink, which a container build repoints to a Linux binary. On Windows
  the launcher runs a snapshot copy, because a running executable is locked (docs/platforms/windows.md).
- ⚠️ Measure first: `tests/` holds about 2,500 fixtures that no manifest owns, and the server analyzes those as one
  program up to its 500-file cap.
- Move the procedures that live only in assistant memory into the repo. The file-triggered ones fit a
  `paths:`-scoped skill under `.claude/skills/`: an edit to `src/kama.y` means four artifacts and a pins commit. The
  rest belong in AGENTS.md.

**Decisions owed** (the recommendation first in each):
1. **Claude Code's channel:** the `kama` plugin with `kama lsp` and the skill (rec.) · a hook · an MCP server.
2. **The skill's text:** printed by the installed compiler at load (rec.) · a copy generated per release.
3. **Where the plugin lives:** this repo's root marketplace (rec.) · a separate repo · a marketplace file on
   kama-lang.org. And whether to submit it to Anthropic's directory after it has run for one release.
4. **The Agent Skills path:** `--skill` also writes `.agents/skills/kama/` (rec.) · Claude only.
5. **Symbol-named `kama query` modes:** schedule with this row (rec.) · positions only.
6. **Call hierarchy:** pull KR-33's residual into this work (rec.) · leave it in KR-33.
7. **MCP:** a non-goal, as argued above (rec.) · schedule `kama mcp`, with `--instructions` printing the skill as
   gopls does.
8. **Skills shipped in packages:** a kama package carries `skills/`, and `kama agents install` gathers its
   dependencies' skills into `.agents/skills/<package>/`, as Dart does. Rec.: file it as its own row, because a
   package author's knowledge, such as how to use a vendored library safely, is exactly what an agent lacks · a
   non-goal.
9. **This repo:** dogfood the plugin against the in-tree compiler and move memory-only procedures into the repo
   (rec.) · leave it as it is.

**Size once decided: L.** The plugin and marketplace (S); `print --body`, the `.agents/skills` path and the `list`
text (S); symbol-named modes (M); call hierarchy (M); the guard (S); the dogfood launcher and its measurement (S).

- **`kama describe --json` (KR-84)** — GOALS §7's "other half" of self-description: the language surface
  itself (keywords, kinds, builtins, attributes, grammar) as data, independent of any source file, so an
  agent or an editor reads it rather than scraping KEYWORDS.md. `kama query --json` is the program half and
  ships. Unscheduled; GOALS.md names this row.

- **`kama stats <op>` — SHIPPED 2026-09-16, kept for the two things it measured.** Asked for as
  "kama diagnostics"; ⚠️ **that name was taken** — *diagnostics* means compiler errors and warnings
  everywhere in this repo, and `kama query --diagnostics` is a shipped mode. `kama stats` it is. The record
  is `kama --help` and [agents.md](agents.md) § *`kama stats`*; the guard is `tools/check-stats.sh`, whose
  every line count is verified by hand against a fixture built out of the traps.

  ⚠️ **Two things the implementation measured, both of which had been asserted wrongly first.**
  1. **A declaration's `endLine` is its SIGNATURE, not its end.** kama.ast.h says the span is "approximate
     for multi-token nodes (bison lookahead skew)", and the cost is concrete: every function ranked as ONE
     line. The body's `block` — and a type's last member — reduce after the closing brace and carry the
     real end, so the span is the furthest of the three. Anything else reading these spans wants the same
     treatment.
  2. **A string's start line was not where the lexer thought it was.** `SAVE_TOKEN_FROM_BUFFER` marked the
     range from `codeGenContext->line`, which the string-start rules never set — so it held the line of
     some EARLIER token, and every blank line between was counted as code. The string openers now mark
     their own line and each in-string newline marks its own, which is also what makes a blank line inside
     a multi-line string correctly CODE.

  Classification lives in the lexer (`kama.l` MARK_LINE -> `CompilationUnit::lineFlags`, one byte per line,
  because this is the per-token path the language server walks on every keystroke). A type counts once
  under its KIND with `generic` as a SUBSET — counting a `type value Box<T>` as both a value and a generic
  made the kind column not add up. Named constructors are method nodes carrying `isCtor`, not
  `ClassConstructorDeclarationNode`, and a `contract`'s members are methods like any other.

- **AI/agent tooling — SHIPPED.** `kama query --search NAME` / `--diagnostics` / `--json`, the
  `kama agents` command, and the `AGENTS.md` it writes into a project. Record: [agents.md](agents.md);
  guarded by `tools/check-agents.sh`. Residuals, none blocking:
  - **No call hierarchy (`callers-of`).** The type hierarchy shipped as go-to-implementation (`--impl`,
    `textDocument/implementation`, `0.9.577`): what implements a contract or one of its members.
  - **No stdin / unsaved-buffer mode.** Every query reads the file from disk, so an agent cannot ask
    about an edit it has not written out. The LSP can; the CLI deliberately cannot.
  - **A `fnptr` signature type is not in the index** (measured `0.9.463`, found beside the fixture that indexed compile-time names).
    `fnptr int32 Op(int32 x);` has no definition site: its declaration answers nothing, a use is `unresolved`
    in `--coverage`, and a label in a call through one (`g_op(x: 1)`) is indexed nowhere. A rename is
    REFUSED, not half-applied — there is no def site to offer F2 on — so this is navigation, not data loss.
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
  - **A type argument's span runs into a NAMED size.** In `InlineArray<int32>#(LIMIT) a`, hover and
    go-to-definition on the first two letters of `LIMIT` answer `int32`; with a literal (`#(4)`) the `int32`
    span is exact (measured `0.9.463`, and so on `0.9.461`). References and rename use `LIMIT`'s own span,
    which is exact, so an edit is never affected — only what the cursor lands on.
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
- **The debugger shows kama, not the emitted C — SHIPPED `0.9.412`–`0.9.415`, kept for what it
  established.** Record: SPEC § *C names* (the `kama demangle` surface), [GOALS.md](GOALS.md) §
  *Production-grade build & debugging*, `editor/vscode/README.md`. Three layers, because no one of them
  can reach what the others do: `include/kama_lldb.py` renders VALUES (LLDB owns a value's children and
  summary); `editor/vscode/names.js` rewrites NAMES in the DAP traffic (locals, the call stack and watch
  come from the debug info, which no formatter touches); and `kama demangle` is what either can ask,
  because reversing a generic instance needs the resolved program.
  ⛔ **A SUBCOMMAND, not a name map** (maintainer, 2026-09-20) — a map is a record that can diverge from
  the binary being debugged, and a stale one misnames a frame SILENTLY.
  Four findings worth not rediscovering, each measured rather than assumed:
  - **An LLDB category defined `--language c` never applies.** Every provider registers, the category
    lists as enabled, and `frame variable` still prints raw structs. The category must be unscoped.
  - **A summary REPLACES a value's children** unless added with `-e`.
  - **Matching `^k_` catches only loose files.** `qualify()` has four scope shapes, and an exported
    module type (`std__collections__…`) carries no `k_` in its type name at all — only in its members.
  - **The DAP batch must be FRONT-LOADED**, which refines how this row was originally framed.
    `onDidSendMessage` is synchronous, so it cannot ask `kama demangle` anything; the one async window
    is `createDebugAdapterTracker`, which may return a Thenable.
  ⚠️ **Residual risk, and what it is measured at.** An extension may only register a
  `DebugAdapterDescriptorFactory` for a debug type IT defines, and `lldb` is CodeLLDB's — so the name
  layer is a TRACKER, which is documented to OBSERVE. It works because VS Code does not clone the
  message before the tracker sees it, which is behaviour rather than contract.
  ✅ **Verified live 2026-09-20 on VS Code 1.125.1 + CodeLLDB 1.12.3** (macOS): stopped in a `.kama`,
  the Call Stack read `probe`/`main` and Variables read `s = "hello"`, `pt = (x = 3, y = 4)`. So the
  approach is sound as shipped. If a future VS Code clones first, names revert to the C spelling — a
  degradation, not a break — and the fallback is a descriptor-factory PROXY in front of CodeLLDB, which
  means owning the adapter process and is why it was not the first choice.
  ⚠️ `tools/check-extension-names.sh` cannot see that regression: it asserts the rewriting RULES, not
  that VS Code honours them. Only a live session can, so re-run the manual check when the editor
  updates.
  ⚠️ It also turned up a defect underneath it — a macOS parallel debug build emitted no `.dSYM`, so its
  Mach-O debug map pointed at deleted objects and the binary could not be debugged AT ALL. Fixed
  `0.9.414`; `tools/check-debug-info.sh` drives both link arms. The Windows box has no `lldb` and cannot
  gate any of this.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
- **Package manager (ecosystem foundation).** A first-class dependency manager + registry so libraries distribute
  without vendoring — the point at which cross-package conformance coherence (SPEC § *`type adapter`*) becomes load-bearing.
  User docs (including the registry protocol a host must serve): [packages.md](packages.md). What remains is
  hosted-services and ops work:
  - **Official vs community packages — DECIDED and SHIPPED (2026-09-06).** The `@kama` scope is the mark
    and the channel (`packages.md` § Scopes says so); `@kama`/`@std` get reserved the day M3.3's host
    exists. **`@kama/sodium` shipped** — libsodium 1.0.20 vendored through
    `csources`/`cincludes`, six modules, proven native (debug/release), wasm and through a file-registry
    publish→install round trip. What it forced in-tree: `kama seed --license`, the `cincludes` key,
    the move-only propagation fix, `csources` as gnu11, `std::digest`; what it rowed: the four rows above
    NOW and the member-visibility question (a non-goal since `0.9.646`, SPEC § *Contracts*). Its AGENTS.md § "This package" is the first draft of
    the library-kind guidance addendum.

      **Decided direction:** follow where the ecosystem landed rather than per-developer signing keys.
      Go ships no package signatures at all and leans on the `sum.golang.org` transparency log; PyPI
      *removed* PGP in 2023 (almost nobody verified) and replaced it with OIDC Trusted Publishing +
      attestations; npm did the same via sigstore provenance; crates.io ships checksums only. So:
      **(a)** near-term, an allowed-signers set — real `ssh-keygen -Y verify` against a configured trust
      set, plus closing the warm-store and git-dep gaps; **(b)** then CI/OIDC provenance recorded in a
      transparency log, at which point verification becomes mandatory. Both gate on a live registry, since
      mandatory verification is meaningless before one exists.
  - **Where the official registry keeps its bytes — DECIDED 2026-09-27.** The index (`<pkg>/index.json`) lives
    in `cosmic-canopy/kama-registry` and deploys to Pages, where every change is a reviewed commit; the tarballs
    live in the R2 bucket `kama-registry-tarballs` behind `dl.kama-lang.org` (`provisioning/pages/main.tf`),
    with an indefinite bucket lock, so write-once is a property of the storage. The first version shipped with
    its tarball in git, which does not scale: a permanent version grows every clone forever, and Pages refuses
    any file over 25 MiB. A `_redirects` rule sends each tarball path to the bucket — after a rule that keeps
    `index.json` on Pages, since a redirect beats a static file (measured) — so index entries stay relative and
    a lockfile never records the bucket's host. **R2 over Backblaze B2**, the maintainer's call on these
    numbers (both vendors' pages, 2026-09-27): past the shared 10 GB free tier B2 stores for $6.95/TB against
    R2's $15/TB, and its egress is free through Cloudflare; R2 egress is free unconditionally, it sits in the
    same account, token and OpenTofu state as everything else, and its bucket lock is native. At today's size
    both cost nothing. Revisit if the registry reaches terabytes (engine-track asset packages, say): the
    redirect makes the move one line.
  - **Secret scanning by content, registry-side.** `kama publish` and the registry's `tools/check.py` both
    refuse secret-shaped file NAMES; a key pasted into `config.json` passes both. A content scanner
    (gitleaks-style) in the registry's CI is the second layer, and host-side it covers every compiler.
- **Longer-term — a "node.js-class" application framework in kama.** A fast, low-overhead server/app framework
  (HTTP already dogfooded via `examples/httpd`), aiming to beat the Node/Deno overhead profile on the no-GC/AOT
  (or VM-scripted) runtime — the flagship *application* of the language + package manager + scripting tiers
  together. See [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md). Aspirational, post-ecosystem.

### Publish from Perforce and other VCSs (KR-101)

The publish seam's second implementation. `kama publish` asks source control four questions (`vcsSnapshot` and
`vcsRead` in `src/kama.driver.cpp`; the rule is [packages.md](packages.md) § *Publishing*): which files are
versioned under the package directory, is the working copy exactly that revision, what is the revision, and what
bytes did it record for each file. Perforce answers them with `p4 have` over the package path; `p4 opened`, plus
`p4 status` for edits made without opening the file; the have-changelist (recorded as `"revision": "p4:<n>"`);
and `p4 print -k` for the depot bytes. The bytes must be the depot's for the reason git's are the committed
ones: a client's `LineEnd` rewrites line endings in the workspace, and `+k` filetypes expand keywords there, so
the workspace is different bytes on different machines. Perforce has its own `.p4ignore`, so "an ignored secret
never ships" carries over, and `publish.exclude` and the secret-name check sit above the seam unchanged.
Mercurial (`hg files` / `hg status` / `hg cat`) and Subversion (`svn ls -R` / `svn status` / `svn cat`) fit the
same shape. Perforce comes first because it is the game-studio default, and studios keep packages as
subdirectories of one large depot — which is why the seam scopes the file list to the package directory. Not
buildable blind: it needs a Perforce server to test against.

<a id="s10-ext-old-prefix"></a>

### Retire the extension's `k_F` (KR-305)

Kama 0.9.818 moved a file's private names in C from `k_F<stem>__` to `kF_<stem>__`. The extension's demangler and its symbol scan read one spelling at a time, and the extension updates by itself where a
compiler does not, so extension 0.9.8 showed `Fapp__probe` for a program built by the released compiler and 0.9.7 shows `kF_app::probe` for one built by a newer. 0.9.9 reads both, by the maintainer's
ruling (2026-11-03): for now, and not to stay.

**The trigger: the second compiler release that writes `kF_`.** By then a project that has moved has had a release to move from, and one pinned to a compiler before 0.9.818 sees `Fapp__probe` again, in the debugger
alone: it is a cosmetic cost, on a pre-1.0 compiler, and it is what retiring it means. The work is `names.js`'s regex, `extension.js`'s scan, the three assertions in `check-extension-names`, and an extension
version bump. **`v0.9.841` (2026-11-04) is the first release that writes `kF_`, so the next compiler release is the trigger.**

<a id="s10-api-reference"></a>

### An API reference for a package (KR-268)

Filed 2026-10-30. A package cannot generate its API reference: `kama query --symbols`/`--type` report neither a
declaration's doc comment nor whether it is exported, and `kama doc` is an unknown subcommand. Rust has `rustdoc` and
docs.rs, Go `go doc` and pkg.go.dev, Zig `-femit-docs`. **Recommended:** the two fields in `kama query`'s JSON first,
since it answers from what the compiler resolved and a renderer outside the compiler then gets them right; then a
`kama doc` renderer, whose shape, and whether the registry hosts its output, wants its own brief. Beside KR-40.

<a id="s10-bench-guard"></a>

### No guard compiles `bench/` (KR-321)

Found 2026-11-04: `bench/fiber/coldswitch.kama` no longer compiled, and nothing said so until a later session needed it (fixed then). `run_tests.sh` reaches `tests/` and `tools/check-*.sh` reach what each names, and nothing reaches `bench/`. All 18 programs there pass `kama check` on `0.9.845`, so a guard is one loop over `kama check`. Building their C too would catch more, and costs a compile per program in a gate already measured in minutes; decide when writing it.

<a id="s10-csources-optimize"></a>

### A package caps the optimization of its own C (KR-269)

Filed 2026-10-30. A package that vendors constant-time C cannot keep it at `-O2` or `-Os`: a release build compiles
every `csources` file at the root's level (`-O3`), a dependency's `cflags` reach every C file in the consumer's build,
and a `csources` entry takes only `path` and `compileFor` (refused at `kama.driver.cpp` ~3569). docs/targets.md rules
that only the root manifest's level applies, so this touches a ruled rule. The plumbing exists: each C input carries
its own optimize flag, and each `csources` file is its own translation unit. Cargo sets a level per package from the
root (`[profile.release.package.<name>] opt-level`); CMake per source (`COMPILE_OPTIONS`). **Ruling owed:** a ceiling
the package sets on its own entries (a cap, never a floor, so it cannot slow the consumer's code), or Cargo's model.
