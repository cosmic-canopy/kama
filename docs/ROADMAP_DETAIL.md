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

- **`std::io` transform adapters: compression and archives (KR-5).** kama's package format is a `.tar.gz`, and
  since `0.9.452` the compiler writes it itself — `src/kama.archive.cpp`, deflate and gzip from RFC 1951/1952
  plus POSIX ustar/pax — because the system `gzip` compresses the same input to different bytes on different
  machines (measured: Apple gzip 479 vs GNU gzip 1.12) and a registry version is identified by its sha256. That
  writer is C++ and the compiler cannot run kama, so it serves `kama publish` and nothing else. The open
  question the maintainer raised (2026-09-26): should kama programs get the same from `std`, or from a package?
  For `std`: deflate and tar are frozen, patent-free standards (Go and Python ship both); kama's own package
  format already IS one, so a registry or package tool written in kama needs it; gzip `Content-Encoding` for
  `examples/httpd` and the web-framework direction, and PNG's inflate on the engine track, are known consumers;
  an unused stdlib module costs a program nothing (measured when the modular stdlib was declared a non-goal, `0.9.360`). A package fits a format that churns
  (zstd, brotli). Needs the inflate half too — the compiler has only a compressor. Unscoped until the verdict.

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
   output today; streaming a running child's stdout as it arrives is the piece left. **And a non-blocking `File`**
   (filed 2026-10-14): `kama_set_nonblocking` exists for POSIX descriptors, but `File` has no non-blocking mode, and
   on Windows both it and the `Poller` reach sockets only, so a child's pipe cannot be read without blocking there.
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
     `0.9.624` (`Stdio::Handle`), so the pair is scheduled: KR-178; (a
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

<a id="s1-unix-pair"></a>

### `UnixStream.pair()` (KR-178)

Filed 2026-10-15, when `Stdio::Handle(descriptor:)` shipped (`0.9.624`) and met the condition the net verdicts set
for it: a connected pair of local sockets, one end kept and one handed to a child as a stream — the shape a parent
uses to talk to a helper process both ways, without a path on disk. **Scheduled.** POSIX: `socketpair(AF_UNIX,
SOCK_STREAM | SOCK_CLOEXEC)` (the `FD_CLOEXEC` gate on macOS, as every descriptor kama makes). Windows has no
`socketpair`, and by the platform-deltas rule its twin is the same `pair()` over a listener bound to a private path,
one connect and one accept, the path removed at once. On Windows a socket is no child's stream (`Stdio` documents
it: the handle is overlapped), so there a pair serves isolates, not a child's stdin. Windows AF_UNIX is stream-only
and passes no descriptors (Microsoft's "AF_UNIX comes to Windows", 2017), so the Windows pair is a stream pair only.

**Where (surveyed on `0.9.630`).** Still missing: `UnixStream.pair()` is "no constructor `pair`".

The child half already works. An accepted Unix stream end, handed to a child as stdin with a `duplicateDescriptor()`
as stdout, round-trips "ping" and then EOF (exit 42). What is missing is a pair with no path on disk.

The seam is `kama_unix_pair(int32 datagram, isize* a, isize* b)` beside `kama_pipe` in kama_os.h:

- Linux: `socketpair(…|SOCK_CLOEXEC)`.
- macOS: under the fork gate (`kama__cloexec_enter`, `socketpair`, mark both ends, `kama__cloexec_leave`), with
  `kama__nosigpipe` on each end.
- emscripten: a plain `socketpair`.
- Windows has AF_UNIX but no `socketpair`. A twin builds the pair from a private path:
  1. build the path from `GetTempPathW`, the pid and a counter, retrying on `AddrInUse`;
  2. `listen(1)`, a blocking connect, `DeleteFileW`, accept, close the listener;
  3. check that `SIO_AF_UNIX_GETPEERPID` is our own pid.

  This is unmeasured until Windows CI runs it: a connect completing on a 1-deep queue before accept, and the queued
  connection surviving the delete.

In lib/std/net/unix.kama the pair type needs a `friend` for `UnixStream`'s private `make` (~231) and for
`UnixDatagram`'s `own` (~461).

Fixtures:

- `net_unix_pair`: round trip both ways, unnamed ends, the peer pid is ours, dropping one end gives EOF on the other
  and then `BrokenPipe`, and one end handed to an isolate.
- `proc_unix_pair`: an end as a child's stdin/stdout, plus the close-on-exec proof.
- A datagram pair case in `net_unix_datagram.d`.

Traps:

- macOS caps a Unix datagram at 2048 bytes and a stream's send buffer at 8192. Keep payloads small, or a parent writing
  to a `cat` it is not reading deadlocks.
- On macOS `SO_NOSIGPIPE` travels into the child, which then gets EPIPE where Linux raises SIGPIPE. Document it.

Size **M**: the POSIX half is S, and the Windows twin needs a CI round.

**Decided 2026-10-16** (the maintainer took (a) for both):

1. The shape.
   - **(a), decided:** a `UnixStreamPair` resource with `ctor Result<UnixStreamPair, IoError> open()` and
     `ref UnixStream first()`/`second()`, the way `Process` hands out `ref File stdin()`. An end leaves through the
     existing `takeDescriptor()`. Prototyped and run.
   - (b) `UnixStream.pair(other: out UnixStream)`. An `out` must be filled on the `Err` path too, so a failed pair would
     hand back an empty stream.
   - (c) A `static fn`: a second way to construct.
2. Windows.
   - **(a), decided:** the listener twin. It is the established emulation (openconnect's `dumb_socketpair` does the
     same over AF_UNIX on Windows 10+; Python's `socketpair()` does it over TCP), kept on the right family, with the
     peer-pid check refusing an intruder who connects first. Abstract (pathless) sockets do not work on Windows, so
     the brief on-disk path is unavoidable.
   - (b) Refuse at run time, which breaks the platform-deltas rule.
   - (c) An AF_INET loopback pair: the wrong socket behind `UnixStream`, so `peerCredentials` breaks.

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

<a id="s2"></a>

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The
language-completeness residual is **closed**; what remains here is genuinely later-track or opt-in.

<a id="s2-borrow-any-place"></a>

### `borrow` names any place for a block (KR-123)

A place is second-class, so there is no `ref` local (`0.9.539` says so in its message). A method that works on one
element of a container therefore repeats the accessor at every use (`this.rig.look(k: look)`), or moves the work
into a free function so the element can be a `const ref` parameter. `borrow` is kama's construct for naming a
borrowed thing for a block. Today it opens only an argument-less mint of a `@viewable` contract. **Design:**
`borrow <place> as L { … }` for any place: a place-returning call with arguments, an element, or a field. `L` is
that place, `const ref` when the place is read-only. Its host is frozen for the block by the same prefix test a
view window uses, so `L` cannot dangle and nothing escapes. No lifetimes, no stored borrows, one construct. The
`0.9.539` message for a `ref` local then points here.

**Where (surveyed on `0.9.630`).** It reproduces:

- `borrow m.getRefMut(key: "a") as v { … }` is refused with "a mint takes no arguments … narrow inside it with
  `.slice(…)`", which is the wrong advice for a `ref` call. Two "cannot resolve `v`" cascades follow.
- `borrow a[0] as e` and `borrow o.inner as i` are refused with "`borrow` binds a mint call".

**No grammar change is needed:** `borrow_binding` already takes any `primary_expression` (kama.y ~1524), and
tree-sitter takes any `_expression`.

Sites, in kama.cemit.cpp unless noted:

- `emitBorrow` (~6891–7000) splits its host three ways: a granted nullary mint (as today), a place, or a refusal.
- A new `placeRoots(expr)` beside `viewRootPlace` (~25386).
- The temporary relocation that `emitForeach` uses (~18135/18340).
- `scanStmtForGenerics` (~16444) gives the alias a type node.
- In kama.y: the `ref`-local note (~3021), and the comment over `borrow_statement` (~1508).

Steps:

1. The alias joins `_refParams`, so it is judged by its host's root through `_refBindingOperand`, as a `foreach (ref …)`
   and a borrowing-`match` binding are since `0.9.631` (SPEC *Writing a collection in kama*, place-returning methods).
2. Accept three hosts: a field path, an element `x[i]`, and a call returning `ref`/`const ref`, with any arguments.
3. Freeze every root:
   - the receiver place;
   - every `ref`/`const ref` argument's place (a `const ref` result may legally point at a `const ref` argument —
     probed);
   - the container of an element.
4. Lower the alias as `T* k_L = &(<place>);`, with the call emitted once. Argument temporaries are declared inside the
   window and dropped when it closes. `L` joins `_constLocals` when the place is read-only.
5. Give each refusal its own sentence, and no cascade after one:
   - a value call or a temporary: "not a place, bind it";
   - a view-returning call with arguments keeps "a mint takes no arguments".
6. Point the `0.9.539` `ref`-local message at `borrow xs[0] as first { … }`.
7. Docs: SPEC window rule 1 (~384) and ~3518. Regenerate `docs/grammar.bnf` for the comment.
8. Fixtures:
   - a positive `borrow_place`, native and wasm: a map key argument, an element, a field path, a two-`ref` free
     function, a `const` alias, the alias passed by `ref`, a nested window, a disjoint sibling write, a temporary key;
   - xfails: a frozen receiver (`m.put`), a frozen `ref` argument, a temporary host, a value call, a write through a
     const alias, a `give` of the alias, and a `fn ref` returning an alias rooted in a local;
   - update `borrow_bare_host`'s message.

Size **M**.

**Decided 2026-10-16** (the maintainer):

0. **`borrow <place>`, not a `ref` local.** A `ref` local (`ref Joint j = this.rig.joint(name: n);`) is safe only if
   its host is frozen from the declaration to `j`'s last use — a region nobody wrote. Rust infers it (lifetimes and
   a borrow checker); C++ does not check it (`auto& j = …` dangles); C#'s ref locals lean on a GC and cannot point
   into a `List<T>`; Swift has none. kama has no lifetimes, so the frozen region is the visible block, the same rule
   a view window already uses. The `0.9.539` `ref`-local refusal stays and points here.
1. A bare local host, `borrow a as v`, is **refused**: `a` is already a name for that place.
2. A call host freezes **the receiver and every `ref`/`const ref` argument**, since the signature lets the result
   point at any of them. Freezing the receiver only is unsound for a free `fn ref`.
3. **No marker** on the alias: `L` is `const ref` when the place is read-only.

**Tests and docs, exhaustive (the maintainer asked).** A positive for every host kind (a mint, a `ref`/`const ref`
call with arguments, a free `fn ref` over two `ref` arguments, an element, a nested element, a field path) and every
statement position (fn and method bodies, a loop body left by `break`/`continue`/`return`, a `match` arm, a nested
window, several bindings, a generic body, `scope`/`parallel_for` bodies, a `comptime fn` body under the one
rule); an xfail for every refused host and every frozen operation, plus overlapping bindings and the same place
nested. SPEC's *The window* is rewritten around "a place", and the new example joins the positives
`tests/idioms_kama_way.kama` compiles.

<a id="s2-view-destructor"></a>

### A borrow window that runs code when it closes (KR-124)

A database transaction wants a scope that ends it on every path, rolling back unless the code committed. Other
clients use a guard that borrows the connection (Rust, C#) or a block or closure (Python, Go). kama has no stored
borrows, so a guard cannot hold the connection. Capturing closures are sized but not scheduled (below), and
their captures are by move or by `Shared` only, so one could not borrow the connection either. The window is the
nearest thing, and a view cannot run anything when it closes. **Design:** a `type view` may declare a destructor. The window that
minted the view runs it when the window closes, on every path out of the block, including `return`, `break` and
`continue`. Such a view is not `Copyable`, and its alias passes `ref`/`const ref`, so the destructor runs exactly
once. Then `borrow conn.begin() as tx { …; tx.commit(); }` rolls back unless `commit` ran.

**Where (surveyed on `0.9.630`).** It reproduces: a `type view Tx` with `~Tx()` gets "a `view` … may not declare a
`~dtor`" plus three cascades, because a view is registered as `TypeKind::Value` (~11374).

Every exit path already works. A `borrow` lowers to a plain C block, and `emitScopeCleanup` runs at its end and on
`return`, `break` and `continue`; a resource local inside a window was dropped in order (probed). A trap aborts, so no
destructor runs there, the same as for a resource.

Sites:

- the `collectClasses` dtor arm (~12000), `computeDestructible` (~20696), and the `__dtor` emission gate (~33563);
- `emitBorrow`, and `rejectViewByRef` (~21718);
- the local, return and generic-argument checks;
- `recordDestructibleOwner` (~6513), which feeds `@onPanic`.

Design A, step by step:

1. Allow `~dtor` on a view under a new `closesWindow` flag. The view stays non-destructible and owns nothing, so the
   value/owning checks and collection drops are untouched. Emit `__dtor` when `destructible || closesWindow`.
2. `emitBorrow` registers a closing alias as a scope local, plus `recordDestructibleOwner`, so the destructor runs once
   on every path, after the body's locals.
3. Pin the view to its window. Each of these compiles today and must be refused for a closing view:
   - a mint outside a `borrow` host (`c.begin().commit()`);
   - a local of the view type;
   - a by-value parameter;
   - a return, other than from the mint itself;
   - a generic argument;
   - a `give` of the alias.
4. `rejectViewByRef` narrows to `ref`/`const ref` passing, with no assignment to the parameter.
5. Refuse a closing view as a `foreach`/`parallel_for` iterator.
6. Docs: SPEC's view bullet (~4031), the window section, `ref`/`out` (~435), RAII; TYPE_MODEL ~147.
7. Fixtures:
   - delete xfail `view_dtor`;
   - a positive `borrow_view_closes`: commit versus rollback across end, `return`, `break` and `continue`, and the
     order of several bindings;
   - one xfail per refusal, and an `@onPanic` region that reaches a closing window.

Size **M**. There is no grammar change: `~Tx()` already parses.

**Decided 2026-10-16** (the maintainer took (A) and `ref`/`const ref`). Today a view may not declare `~dtor` at all;
(A) is C#'s `ref struct` + `Dispose` and Rust's guard + `Drop`, without lifetimes. A trap aborts without running it,
as for every destructor; a database server rolls back an uncommitted transaction when the connection drops.

1. The design.
   - **(A), decided:** a view's `~dtor` runs when its window closes, and the view is pinned to the window. `~dtor`
     stays the one scope-exit mechanism, and the view still owns no memory (C#'s `ref struct` with `Dispose`).
   - (C) The `@viewable` grant names a closing method on the host: a second mechanism, and a new spelling.
   - (D) A non-goal: the guard owns a `Shared<Conn>`, so every connection pays a heap allocation and a count.
2. Passing a closing view to a helper: **`ref`/`const ref` only,** so a helper can use the transaction without
   ending it, and the destructor runs exactly once, at the window.

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

<a id="s2-fnptr-type-arg"></a>

### An `fnptr` type is not a type argument (KR-107)

Found 2026-09-30 while probing the one default rule (`0.9.480`). SPEC calls a stored `fnptr` "the callback-registry shape",
and a registry is a collection — but `DynamicArray<Op>` for `fnptr int32 Op(int32 x);` does not instantiate:
`DynamicArray::<Op>.empty()` reports *"cannot tell which `DynamicArray` to construct"* (the turbofish IS
written), `.add(item: twice)` reports the instance *"has no method `add`"*, and the element store trips *"a
FunctionPtr binds a free function name or another FunctionPtr"*. `Op` itself resolves — an unknown name there
reports "unknown type" first — so it is not KR-21's unresolved-generic-argument item, though the first message
is the same. `FixedArray<Op>`, `Box<Op>` (any user generic) and `BindableFunctionPtr<Op>` as an argument all fail
the same way. Close means a generic type instantiates over an `fnptr` element, including the default rule's answer
for it (none — an `fnptr` is non-null, 0.9.480), with a fixture holding a registry of callbacks.

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
  - **`std::gpu` has no kama module.** The `kama_gpu.h` seam exists but programs `extern` the WebGPU C API
    raw; an idiomatic wrapper is library work on the engine track (§8).

  Decided NOT to add a convenience-import of the common containers — explicit per-symbol imports stay.
- **Format/interpolation follow-ups (on the shipped `std::fmt` substrate).** Interpolation, format specifiers,
  `@generate(Formattable)`, and tagged strings all ship (SPEC), on every subject including a generic and an
  `enum` (0.9.246 — see *Derive follow-ons* above). Settled by the audit (2026-09-07). Shipped 0.9.226: combining a
  base marker with width/flags (`${n:08x}`), a custom fill character, center-align (`^`); a `${x:?}`-routed
  `@generate(Debug)` (spec hook already exists);
  per-derive `@skip(Formattable)` / `@skip(Serializable)` for redaction (today `@skip` is one shared boolean —
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

<a id="s4-json-value"></a>

### An untyped JSON value (KR-110)

**Scheduled, not optional.** The typed path cannot read a document whose keys are data. glTF keeps
`extensions` and `extras` as open objects. A web API often answers with a map keyed by ids. A config file may
carry blocks for tools its reader has never heard of. A program that reads glTF today declares a schema for
everything it touches, and makes every optional glTF member `Optional` or `@field(default)`. It still cannot
reach an extension it did not predict. Every mainstream standard library that has JSON also has the dynamic
half: Go decodes into `map[string]any`, Python into `dict`, .NET has `JsonDocument`/`JsonNode`, and Swift has
`JSONSerialization`. Rust leaves it to serde_json's `Value`.

**Shape, to decide before code:**
- A tagged enum, `Null`, `Bool(bool)`, `Number(…)`, `String(string)`, `Array(DynamicArray<JsonValue>)` and
  `Object(…)`, holding itself through a collection. That shape copies and drops correctly since `0.9.531`
  (`tests/enum_copy_settled`).
- `Object` keeps insertion order, as JSON text does, so a read and a write round-trip.
- **Numbers are the real question.** A `float64` loses integers past 2^53. Keeping the number's text and
  converting on read, as `serde_json`'s `arbitrary_precision` and .NET's `JsonElement` do, keeps every
  number exact, at the cost of making `Number` a `string` underneath.
- A parse (`parseJson(text:) -> Result<JsonValue, DeError>`) and a write, in the module of the typed back end.
- Whether a typed schema may hold an opaque subtree: a `JsonValue` field in a `@generate(Deserializable)`
  type, which is how a glTF reader keeps `extensions` without modelling them.

**Where (surveyed on `0.9.630`).** There is no dynamic reader. `JsonDeserializer`'s cursor (`peek`, `numToken`) is
private, and the `Deserializer` contract has no "what comes next" member.

What exists today:

- `Map` is a hash map (open addressing), with no insertion order. `SortedMap` is ordered by key.
- Map's lookup convention is the one to mirror: `contains`, `getRef` (which panics on an absent key), and `get`
  (an `Optional` copy).

Found on the way and filed separately:

- A recursive enum's copyability depended on payload order, and the tree has exactly that shape — fixed `0.9.636`.
- The integer reader accepted invalid numbers and aborted on overflow, and deep nesting overflowed the stack — fixed
  `0.9.637` (every reader stops at 128 levels with `TooDeep`, which the tree's own parse inherits).

Two more constraints:

- A value-producing `match` cannot return a borrow. `return match (this) { case Arr(items: xs): xs[i]; … }` is "ref T
  result must borrow this"; the statement form works.
- A module `static` cannot hold an owning enum, so there is no static `Null` sentinel for a missing key. Lookup returns
  `Optional`.

Sites: json.kama `numToken`/`readInt`/`readUint`/`skipValue` (~334–555); the prelude's `Serializer` (~360) and
`Deserializer` (~520).

Steps, after KR-186 (`OrderedMap`):

1. Add `JsonValue`, a Copyable `JsonMember { key, value }`, and `JsonNumber` in json.kama.
2. Accessors: `has`/`getRef`/`get`/`at`/`length`/`members()`, and `asInt64`/`asUint64`/`asFloat64` returning
   `Optional`.
3. Fixtures:
   - a byte-exact round trip;
   - 2^63+1 and 1e400;
   - key order, and duplicate keys;
   - a deep-nesting refusal;
   - a typed schema holding a `JsonValue` field;
   - a KBIN-named round trip;
   - an xfail for the positional reader.

Size **M**.

**Decided 2026-10-16** (the maintainer; decision 2 CHANGED from the survey):

1. How a number is held.
   - **(a), decided:** `JsonNumber`, a resource over its exact text. Every number stays exact, and invalid text
     cannot be built.
   - (b) Three variants: int64, uint64, float64.
   - (c) float64 only, which loses integers past 2^53.
2. Object storage: **(c), a new general `std::collections::OrderedMap<K, V>`** (KR-186): `Object` is
   `OrderedMap<string, JsonValue>`. The surveyed (a), a linear list, is O(n²) when a big object is read key by key;
   production libraries index (.NET's `JsonObject` keeps the list and builds a dictionary past 9 properties;
   serde_json's ordered mode is `IndexMap`). Order still round-trips; a duplicate key keeps its first position and
   takes the last value (Python's `dict`), matching the typed reader's last-wins.
3. How the tree is reached.
   - **(a), decided:** `JsonValue` implements `Serializable`/`Deserializable`. That needs three new contract
     members: `peekKind()` and `readNumberText()` on the reader, and `writeNumberText()` on the writer.
     - The parse is `deserializeJsonBuffer::<JsonValue>`, with no second parser.
     - A typed schema can hold a subtree (glTF `extensions`).
     - A positional reader answers `TypeMismatch`.
   - (b) JSON-only `parseJson`/`writeJson`, with no subtree inside a typed schema.

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

<a id="s5-match-binding-copy"></a>

### A `match` binding copies a value payload (KR-131)

Measured on `0.9.560`, after construction stopped copying: `Result<Big, E> r = Big.tryMake(…)` builds in `r`, and
then `match (r) { case Ok(value: b): b.last(); … }` declares `Big b = subject->Ok.value;`, a 16 KB copy (`local`'s
frame is 32,928 bytes at -O0, 16,448 at -O2). The subject is already borrowed by pointer. A binding the arm never
writes, never takes by `ref` and never gives away could be that payload in place: `const Big* b = &subject->Ok.value`
read through, as a `const ref` parameter is. The rules for when an arm's binding owns its value (a `give` subject, a
moved-out payload) decide where that holds. A by-value `foreach` binding is the same copy of each element and takes
the same answer. The semantics stay: a value binding is a copy the body may change (probed: writing one changes
neither the subject nor the element), so this is a lowering, not a language change. The read-in-place applies
only where the body provably never writes the binding.

**Where (surveyed on `0.9.630`).** The `foreach` half is ANSWERED. `foreach (const ref Big b in xs)` (`0.9.605`) copies
nothing. A by-value loop still copies once at -O2 (twice at -O0), because `Iterator<T>.next()` yields by value. Only
another method (`iterConst()`) removes that copy, and that method is the `const ref` loop — it is not a lowering.

The `match` half reproduces: `case Ok(value: b): b.last()` over a `const ref Result<Big, E>` (a 16 KB `Big`) emits
`k_Big k_b = kama_msub2->kama_u.k_Ok.k_value;`. That is one 16 KB copy at -O0, and at -O2 whenever `last` is in
another C unit; clang removes it only when it can see the callee.

Sites, all in kama.cemit.cpp:

- `emitMatchSwitch`: the subject borrow and `subjOwner` (~28755–28860), the binding (`const bool alias = …`, ~28993),
  and the `_refParams`/`_constLocals` marking (~29012).
- The one existing write scan, `emitParallelFor`'s `capWrites` (~7245–7340).
- The freeze machinery: `Scope::FrozenPlace`, `frozenConflict`/`rejectFrozenWrite`, `checkConstWrite`.

Fix:

1. The write scan exists since `0.9.632`: `forEachPlaceUse(stmt, fn, /*writesOnly=*/true)` reports every place a
   statement writes (an assignment or `++`/`--` target, a non-`const fn` receiver, a `ref`/`out` argument, a `give`).
2. Bind the payload in place (`T* b = &slot`) only when all of these hold:
   - the `match` borrows its subject;
   - the payload is a plain struct;
   - the arm never writes, `ref`s or `give`s `b`;
   - the subject is a temporary, a `give` copy, or a local or by-value parameter that the arm never writes.
3. Mark the binding `_constLocals`, and freeze the subject for the arm. A write the scan missed then becomes a compile
   error, never a miscompile.
4. Fixture `match_binding_in_place`. Extend `check-construct-frame.sh`: the -O0 frame, and -O2 with the callee in
   another unit.
5. A use of an owning payload's alias after a write to the subject is refused since `0.9.632` (SPEC *match*,
   "Writing the subject ends its aliases"); the in-place binding must keep that rule.

Narrow this row to the `match` half. Size **M**, 2–3 commits.

**Decided 2026-10-16** (the maintainer took both; a program that compiles today behaves the same — only the C
changes):

1. **A lowering.** It keeps one spelling and copy semantics. The alternative, a
   `case Ok(value: const ref b)` pattern, is a grammar change, and a pattern binding has no type slot.
2. Which subjects qualify: **locals and temporaries only.** The overlap below was a use-after-free on its own (an
   `Optional<DynamicArray<…>>` replaced through `b` while `a`'s payload is read); since `0.9.633` the call
   `two(a: o, b: ref o)` is refused, so a `const ref` parameter no longer overlaps a `ref` one.
   - A static that a callee writes is the remaining case: measured, the copy returns 7, while reading in place
     would read the overwritten payload.
   - That refusal is what widening to `const ref` parameters needed; a static a callee writes still bars statics.

<a id="s5-noheap-contract-slot"></a>

### `@noheap` across a contract slot (KR-39) — surveyed on `0.9.630`

**The generic-bound case is already solved, but nothing pins it.** `@noheap fn int64 measure<T: Shape>(T sh)` lowers
to a direct `measure__Sq(…)` call and builds, and it is refused with a chain when `Sq.area` allocates. No fixture holds
this.

Still refused:

- a local upcast in the body (`Sq q…; Shape sh = q; sh.area()`): "a contract member not declared `@noheap`";
- a contract parameter under `--no-heap`;
- `tests/global_allocator_serde.kama --no-heap`, at the drop of `Owned<Error>` (the `kama_dtor` slot).

**What "knowable" means.** In a whole-program build, every contract or virtual slot can hold only a function that some
emitted vtable instance installed (`const V Impl__as_C = { .k_area = &Impl__area, … }`). Refinement emits its own
`Sp__as_Drawable` (verified). So the callee set is finite and visible, heterogeneous collections included. Only `fnptr`
slots, bound function pointers and runtime-header callbacks stay blind.

Sites:

- `emitInterfaceDispatch` (~32892, the immediate `rejectNoHeapIndirect`);
- `emitIfaceHandleFuncs` (~17674, `kama_dtor`);
- `__vdrop` (~32960) and the virtual sites (~38051, ~38151);
- `buildCallGraph` (~31497; the member-call facts at ~31690) and `checkNoHeapTransitive` (~31762);
- kama_runtime.h ~279, beside `KAMA_NOHEAP_SLOT`.

Fix:

1. `#define KAMA_SLOT(V, f) (f)` wraps every unproven slot call (`KAMA_SLOT(<C>_vtbl, recv.kama_vtbl->k_m)`).
   Emission stops refusing, so the walk decides for `@noheap` and `--no-heap` alike.
2. `buildCallGraph` parses the emitted vtable initializers (three emit sites: ~15120, ~32635, ~36392) into a map
   V → slot → targets. The per-TU `static const` duplicates are unioned by V.
3. A `KAMA_SLOT(` adds an edge to every target; a null slot adds none. The hop renders as
   `Shape.area [Sq2] -> Sq2::area`.
4. Fixtures:
   - flip `xfail/noheap_contract_member` to positive;
   - give `noheap_flag_reached_dispatch.d` an allocating implementer;
   - update `noheap_drop_{contract_box,virtual_box}`'s `.msg`;
   - a two-implementer positive, and an xfail where one implementer allocates;
   - a `--no-heap` + pool copy of `global_allocator_serde` that must BUILD;
   - pin the generic-bound case.
5. Rewrite SPEC's *No-heap subset* ("blind seam", "a runtime fact"), and KR-79's "inherits KR-39" note.

Traps:

- Intrinsic implementers go through `__thunk` bodies, which is fine.
- Keep the `!header` exclusion (`kama__loop_step`).
- Without a pool, the serde drop stays refused, now naming the implementer.
- `expose fn measure(Shape sh)` builds, so a host-forged vtable is unchecked C. Say so in SPEC.
- **A display defect seen on the way:** chains print `measure::k_Fk39a2::Sq` and
  `Owned_kama::Error_kama::GlobalAllocator::dtor`, i.e. generic instances are not demangled. Fix it with this row.

Size **M**.

**Decided 2026-10-16** (the maintainer took all three):

1. The callee set: **every vtable instance in the emitted, pruned C** — sound, and the least code (class-hierarchy
   analysis, as clang's `-fwhole-program-vtables` and Java JITs use). The
   alternative is only the vtables named by reached bodies, which needs a fixed point.
2. **The same rule for virtual and destructor slots** — one rule.
3. **Keep `@noheap` on a contract member,** as an optional promise checked at the implementers.

<a id="s5-sizeof-comptime"></a>

### A struct's `sizeof` and `alignof` as a compile-time value (KR-94) — surveyed on `0.9.630`

The current state:

- `comptime usize H = sizeof(Header);` → "`sizeof` folds only for fixed-width scalars…".
- `alignof(int64)` → "does not fold".
- `InlineArray<uint8>#(sizeof(Header))` → "does not fold".
- `comptime assert(cond: sizeof(Header) == 16, …)` works, as `_Static_assert((sizeof(k_Fa__Header)) == (16), …)`.

**GO — decided 2026-10-16.** It finishes ruling 5 (2026-09-24) rather than reversing it. That ruling closed an
INCONSISTENT path: before `0.9.444` the same `comptime usize H = sizeof(Header);` was refused at module scope and built
at type scope by letting C evaluate it, so one declaration meant different things by position. The ruling made it one
rule — kama never guesses a value the target's ABI decides — and the proper unlock, a model the C compiler re-checks,
was filed as this row. Run time costs nothing (the baked number is C's own, and `_Static_assert` emits no code); a
program gains fixed buffers (`InlineArray<uint8>#(sizeof(Header))`), record strides and size classes with no heap, and
`kama check`/the LSP can answer a layout `comptime assert` that today fails only at build. Peers all have it (C/C++
`sizeof`, Rust's `const fn size_of`, Zig `@sizeOf`, Go `unsafe.Sizeof`, D `T.sizeof`) — and all own their layout,
which is why kama's every baked value carries the C cross-check.

Half a layout model already exists:

- `byteViewFieldLayout`/`byteViewLayout` (~21103/21180) handle pointer width, `@packed`, base types,
  `InlineArray`/`Simd`, and enums with a declared integer type.
- `emitBytesDefinition` (~21440) already emits per-field `_Static_assert(offsetof…)`.

Sites:

- Fold sites: `scalarByteSize` (~12257), `constValue`'s `SizeofNode` arm (~12445), `rejectUnfoldableConstArg`
  (~12610), `ctaNeedsCLowering`/`ctaRenderC` (~12645), and the interpreter arm (kama.comptime.cpp ~462).
- Target facts: kama.driver.cpp's `ARCH_` flags (~2576). The named targets MACOS/LINUX/WINDOWS/EMBEDDED carry an empty
  arch (~2759).

Fix:

1. Generalize the byte-view walk into one `layoutOf(type, model)`.
2. A per-arch model: {pointer bytes, int64/float64 alignment cap, enum bytes, `clong`}.
3. Route both fold sites through it.
4. Emit `_Static_assert(sizeof(T) == N && _Alignof(T) == A)` once per baked type, so a wrong model fails the BUILD,
   never the program.
5. Flip these xfails to positive fixtures: `type_comptime_sizeof_aggregate`, `local_comptime_sizeof_aggregate`,
   `type_comptime_alignof`, `alignof_not_foldable`, `sizeof_usize_not_foldable`, `sizeof_clong_not_foldable`,
   `sizeof_const_generic_unfoldable`. Add new xfails for a `type extern` and an unknown arch.
6. Rewrite SPEC ~3388–3400, ~5141–5161 and ~5280, and `ClassInfo::alignN`'s comment (kama.cemit.h ~449, which states
   the opposite policy).
7. Prove it on the wasm leg and with `check-target`; Windows CI covers LLP64.

Traps:

- There are two fold paths: `constValue` and the interpreter.
- `kama check` evaluates each `[--target …]` configuration, so values differ per configuration.
- `transpile` output becomes target-specific.
- `--target WINDOWS` on this Mac means aarch64-windows.
- AVR's `double` already fails the `kama_runtime.h` assertion.

Size **M**, 4–6 commits.

**Decided 2026-10-16** (the maintainer took all six):

1. **GO.** The assertion makes a wrong model a build error.
2. **Every type except `type extern`,** since every kama lowering is structs and unions over scalars, pointers and
   `kama_string`.
3. **Derive the model from the generalized byte-view walk,** not from a second walker beside `cType`.
4. **Assert once per type, beside its C definition.** A generic instance is keyed by its mangled name.
5. An unknown arch (or EMBEDDED without a triple) is **refused, naming the arch.**
6. A variant's tag (a C `enum`) is **modelled as 4 bytes behind the assertion.** A fixed-width tag would change the C
   layout the host header exposes.

### The allocation campaign — opened 2026-09-12, COMPLETE 2026-09-20

What it did, in one paragraph: `kama_alloc`/`kama_free` became the only way heap memory is obtained or
released, delegating to a global allocator that defaults to `malloc`/`free` and that a program can
replace; every raw site in emitted C, the runtime headers, the OS seam and the prelude moved onto them,
which also ended the ~20 blocks allocated by one family and freed by another (correct before only because
every family was libc); error boxing draws from an allocator like every other box; and `--no-heap` judges
what the program REACHES, consistently, rather than every imported body. What it did NOT close is KR-39
(a provable callee behind a contract slot), which it turned from theoretical into measured — see the
serde note under *Non-goals* below; it shares its reach walk with tier 1 of the devirtualization ladder
(KR-23).

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
  ⚠️ What that left unproven is **KR-39**, and this is its first concrete consumer: the same serde
  fixture under `--no-heap` is still refused, because dropping a boxed error dispatches a destructor
  through a contract the walk cannot resolve — the pool is irrelevant to that verdict. KR-39's row used
  to say it waited on this one; the dependency ran the other way.
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
  `View<T>` — see SPEC). Scheduled 2026-10-16: the insertion-ordered `OrderedMap`/`OrderedSet` (KR-186) and a
  bounded inline vector (KR-187). **Not** planned as stdlib: general **linked lists** (mostly a cache anti-pattern in
  data-oriented engines — the useful form is an intrusive free-list / LRU); raw **BSTs** (subsumed by the sorted
  map); **spatial trees** (quadtree/octree/BVH/k-d — engine-specific); **a multimap or multiset** (decided
  2026-10-16: `Map<K, DynamicArray<V>>` answers it, and only C++ ships one); **an LRU cache** (decided 2026-10-16:
  `OrderedMap` plus its move-to-end is the whole of it, Java's `LinkedHashMap` precedent). A concurrent map is §6's
  question, not this list's.

<a id="s5-ordered-map"></a>

### An insertion-ordered map and set — `OrderedMap<K, V>` / `OrderedSet<K>` (KR-186)

Scheduled 2026-10-16 with KR-110, whose JSON `Object` is `OrderedMap<string, JsonValue>`. std orders a map two ways
today: `Map` by hash (no order) and `SortedMap` by key (a B-tree). The third standard order is INSERTION: Java's
`LinkedHashMap`/`LinkedHashSet`, Python's `dict`, .NET 9's `OrderedDictionary<K, V>`, Rust's `indexmap` crate. It is
general-purpose — a config that writes back in the order it was read, a JSON object, a registry listed in
registration order, an LRU (`moveToEnd`) — so it is a std collection, not a JSON detail.

**Shape.** Python's compact dict: an entries array in insertion order plus a hashed index of entry positions, so a
lookup is O(1) and iteration is the entries array. The surface mirrors `Map` (`put`/`get`/`getRef`/`getRefMut`/
`remove`/`contains`/`length`, `values`/`valuesMut`/`valuesConst`/`entries`, `Hasher` and `Allocator` parameters,
serde) plus what order adds: `first`/`last`, and `moveToEnd(key:)`. `put` on an existing key replaces the value and
keeps the position (Python). `OrderedSet<K>` is the same over keys, as `Set` is to `Map`.

**Decisions owed (asked when it is built):** removal — a tombstone compacted on growth (Python; order kept, O(1)),
against a shifting remove (O(n)) or IndexMap's swap-remove (breaks order, so refused for a type named for order);
and whether positional access (`at(index:)`) is offered, which tombstones make O(n) until compaction. Size **M**.

<a id="s5-bounded-vector"></a>

### A bounded inline vector — capacity in place, length 0..N, no heap (KR-187)

Scheduled 2026-10-16. std has a growable array on the heap (`DynamicArray`), a fixed size on the heap
(`FixedArray`) and a fixed length in place (`InlineArray<T>#(N)`, every element live). What it lacks is the growable
list that lives IN a struct or on the stack: capacity `N` in place, length `0..N`, an element constructed only when
it is added — C++26 `std::inplace_vector`, Zig's `BoundedArray`, Rust's `heapless::Vec` (the embedded standard) and
`arrayvec`. It is the natural container for `--no-heap` and the MCU track. A `DynamicArray` over a `BumpAllocator`
only half answers it: growth abandons the old block, and the list is still a pointer, so it cannot sit inside a
`type value` that is copied, sent to an isolate or viewed as bytes.

**Decisions owed (asked when it is built):** the name (`BoundedArray<T>#(N)`, Zig's word for "length bounded by N",
against `InlineDynamicArray`, which composes the two existing words); what a full `add` does (a trap with the
caller's location, as an index miss does, against a `Result` that hands the item back, as `heapless` and Zig do);
and how the uninitialized tail is held (`slot` storage). Size **S–M**.
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

### Declared reachability properties — generalizing what `--no-heap` already proves (KR-79)

**Opened 2026-09-20.** `--no-heap` is not a flag that turns off an allocator. It is a **whole-program
reachability proof**: the compiler walks the call graph it read out of the emitted C, decides whether any
path from the region reaches an allocation, and reports the chain when one does
(`main -> DynamicArray<string>::dtor -> GlobalAllocator::deallocate`). `@noheap` says the same thing about
one function rather than one build. That machinery — the reach walk, the region marker, the chain-shaped
diagnostic, the `@heap extern fn` escape for C the compiler cannot read — is general. What is specific is
the single hardcoded predicate: *does this edge allocate*.

**So kama already ships a proof engine with exactly one theorem in it.** The row is to ask which other
properties are worth declaring, and what the surface for declaring them is.

Candidates, all of which are reachability over the SAME graph and none of which needs a proof assistant:

- **no panic** — nothing on this path can trap or abort. The immediate consumer is a callback that must not
  unwind into foreign code, and an `@onPanic` handler itself.
- **no recursion** — bounded stack. This is the one embedded and real-time users actually ask for, and it is
  a cycle check on a graph already built. Pairs with a computed worst-case frame size later.
  - **The worst-case stack, measured as a real budget (2026-10-04).** A wasm32 game holds its stack to a
    deliberate 64 KB with two guards that each see half of it: a run-time limit sees DEPTH but only on a path
    somebody takes (the break it missed needed a browser to find), and `-Wframe-larger-than=32768
    -Werror=frame-larger-than` sees every FRAME but not the chain under it. A bound per entry point — a frame
    callback, a simulation step, an `@noheap` audio callback — reported as a chain the way `@noheap` reports
    one, would make the whole budget a build-time fact. Its largest frames at `0.9.530` were 21.9 KB (22.9
    native), 12.1 KB and 9.9 KB, and its tests reach 51.6 KB.
  - **The weights already exist.** clang and emcc both write every function's frame with `-fstack-usage` (a
    `.su` file per translation unit: `kamaShape 16400 static` on wasm32), so the edges this walk follows can
    carry the compiler's own numbers — nothing estimated. A call through a `fnptr` or a contract slot is the
    same unknown edge `@noheap` already makes declare itself, and a `dynamic` `.su` entry (a VLA, `alloca`)
    is the same kind of unknown.
- **no blocking** — no syscall that can sleep, for an audio/render callback or an interrupt handler. Needs
  the OS seam to carry the marker, which `@heap extern fn` shows is a solved shape.
- **no unsafe** — nothing reached is an `unsafe fn` or an extern call.

**Why it is worth doing, stated against GOALS.** Each is a property a systems programmer today asserts in a
comment and verifies by reading, which is exactly the class of claim this repo's own house rule refuses to
trust. Each is checkable, cheap (the walk exists and measured below noise when `0.9.401` added header
bodies to it), and reports as a chain that names the edge to fix rather than a yes/no.

**Why it is NOT a proof language.** Prompted by looking at Bend 2, whose `LAWS.bend` is a file of theorems
the compiler re-proves on every change — the same instinct, reached from dependent types. kama should not
take dependent types, an affine no-sharing rule, or a theorem syntax: those buy properties about VALUES,
and everything above is a property about REACHABILITY, which kama can already decide. The overlap is the
idea that a machine-checked invariant belongs in the source rather than in a comment.

**Unscoped — design first.** The open questions are the surface (one marker per property, a parameterized
`@requires(…)`, or a region construct), whether a property is declared per function like `@noheap` or per
build like `--no-heap` or both, how a property crosses a contract slot (the KR-39 problem, which every one
of these inherits verbatim), and whether user-defined properties are a goal or a non-goal. Nothing is
built until that is written down and a verdict recorded here, per the surface-item rule in AGENTS.md.

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
- **`comptime fn` nice-to-haves (deferred).** A per-fn `@steps(…)` budget override; dual-use fallback
  emission (a `comptime fn` called at run time is an error today). Named-arg reorder inside a comptime fn body
  and a local `comptime T X = f();` both work since `0.9.444`. `sizeof` of a fixed-width scalar folds in a
  comptime fn body; a struct's `sizeof` and any `alignof` are refused at every scope — that is KR-94, below.
- **A struct's `sizeof` and any `alignof` as a compile-time value (KR-94).** Since `0.9.444` — the
  maintainer's ruling, 2026-09-24 — no `comptime` at any scope takes a value the target's ABI decides:
  `sizeof` folds for the fixed-width scalars and is refused for a struct, `usize`, `bool` and the rest, and
  `alignof` is refused outright ("`sizeof` folds only for fixed-width scalars … user types have target- or
  layout-dependent size"). Two things already answer part of the need: the value is there at RUN time
  (`sizeof(Box<uint16>)` in a body is C's own answer), and `comptime assert(cond: sizeof(Vertex) == 20, msg:
  …)` is checked at build time by C, for the real target. What neither gives is the size as a VALUE a program
  is built from: `InlineArray<uint8>#(sizeof(Header))`, a record format's fixed stride, an allocator's size
  classes.

  **The unlock** (asked by the maintainer: can it be done? — yes): the compiler computes layouts itself, from
  a per-target model — pointer width and data model (LP64; LLP64 on Windows, where `long` is 32-bit; ILP32
  wasm32; the MCU targets), each primitive's size and alignment, C's struct algorithm (each field at its
  alignment, the size rounded up to the largest), kama's own lowerings (tag-then-union enums, `Optional`,
  `string`, a contract's fat pointer, the smart pointers) and `@packed`/`@align`. Every value it bakes is then
  re-checked by the C compiler that builds the program: the emitted C carries `_Static_assert(sizeof(T) == N,
  …)` (and `_Alignof`) for each type a baked constant read, so a model that disagrees with the target fails
  the BUILD, naming the type, and never the program. `tools/check-target.sh`'s cross-builds then prove the
  model on every triple they build. Still refused after it: a `type extern` (its layout lives in a C header
  kama does not parse) and anything that contains one.

  **Before code:** where the assertions go (per use, or once per type; header or unit); how a generic
  instance's layout is keyed; whether the model sits beside `cType` or is derived from it; targets like AVR,
  where every alignment is 1.
- **Platform tag-type compilation.** The `@compileFor`-gated contract-impl seam is the sanctioned platform-variance
  mechanism (per-platform `type` impls behind a platform-agnostic `contract`, exactly one survives) — NOT
  in-function branching / `#ifdef`. Extending it as new targets land is forward library/driver work.

- **Extended `asm` and `@naked` (KR-83).** `asm("…")` ships as one volatile, full-barrier string with no
  operands (SPEC *Inline assembly*) — enough for `wfi`/`cpsid`/`dsb`, not for reading a register into a kama
  local or for a context switch. The two missing pieces are **operand constraints** (the GCC extended-asm
  `"=r"(x)` shape, spelled with named arguments) and **`@naked` functions** (no prologue/epilogue — an RTOS
  context switch or a reset handler written in kama). SPEC called both "tracked follow-ons" with no row until
  the docs audit; this is the row. Scope it against a real board's needs before choosing a spelling.

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
- **A concurrent map (Java's `ConcurrentHashMap`, Go's `sync.Map`) — noted 2026-10-16, not ruled.** It is the
  shared-memory container the bullet above defers; isolates share through channels and immutable `Shared`. Whether
  the job system (KR-3) wants a channel-fronted or sharded map as a library is decided with that row.

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
scene/material. Depends on the 1.x systems (file I/O for assets, serialization for scenes). The engine *spine*
(buffer/pipeline/binding libraries, renderer) is the engine product. See
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

**What stays in the toolchain: `--webgpu` / `"webgpu": true`** (docs/targets.md). It is the platform half — it finds
the wgpu-native SDK (`tools/fetch-webgpu.sh` or `$KAMA_WGPU_DIR`, with an actionable error when it is missing), adds
its include, library and rpath, and on wasm passes emscripten's own `--use-port=emdawnwebgpu`. Without it every
project would hardcode SDK paths and emcc switches. A dependency cannot turn it on for its consumer, by design, so
`@kama/gpu` should refuse to build — naming the key — when the consumer has not set it, rather than fail on
"webgpu.h not found". **What leaves std now** is the seam itself (KR-188).

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
    webgpu.h. Buffers/bindings/pipelines *as an engine renderer* stay out; the same things *as released handles*
    are in.

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

<a id="s8-seam-out"></a>

### The GPU seam leaves std; the triangle becomes an ordinary project (KR-188)

Decided 2026-10-16 with the package ruling above. Today the driver special-cases one stdlib header: a program that
writes `extern "kama_gpu.h";` gets `lib/std/gpu/kama_gpu.c` compiled into it (as Objective-C on macOS, for the Metal
layer), `-I…/std/gpu` and the Homebrew include dirs, and GLFW plus the platform frameworks linked
(`needsGpu`, kama.driver.cpp ~12985/13276/13402/13515). Only `examples/webgpu/triangle.kama` uses it, and every
compiler binary embeds the seam because it sits under `lib/`.

The move, one commit:
1. `examples/webgpu` gets a `kama.json`: `"webgpu": true`, the seam's C as its own `csources` (the macOS half as a
   `.m`, gated by fact, since csources compiles ObjC since `0.9.380`), and GLFW plus the frameworks in `link`, per OS.
   That is the pattern any project uses for its own platform layer.
2. Delete `lib/std/gpu/` and the driver's `needsGpu` branch, with its include and link lines. `--webgpu` stays as
   it is (§8's opening).
3. `tests/webgpu/smoke.kama` stays: it uses webgpu.h only, with no seam, and is the `--webgpu` link gate.
4. A guard that `kama check`s the example and the smoke for native and wasm on every run (no SDK needed), and builds
   and runs the smoke when the SDK exists, else SKIP — no CI job reaches either today.
5. Docs: docs/targets.md, ENGINE_READINESS.md, the example's README.

When `@kama/gpu` and `@kama/window` exist, the triangle moves onto them. Size **S**.

<a id="s8-gpu-wrapper"></a>

### `@kama/gpu` — the safe WebGPU binding, a first-party package (KR-25) — surveyed on `0.9.630`

A PACKAGE since 2026-10-16 (§8's opening). The survey below was written for a std module; its design and decisions
carry over unchanged, and its "manifest entry" / `check-gpu.sh` steps become the package's own manifest and CI.

The current state:

- `lib/std/gpu` holds `kama_gpu.h` (the web half, static inline) and `kama_gpu.c` (native, GLFW), and no kama code.
  `gpu` is not in `lib/kama.json`, so `import { std::gpu::Device }` gives "cannot resolve module".
- The seam is four functions: `surface` (it creates the window and the surface together; the native window is a file
  static `g_window` that is never destroyed), `present`, `pump` and `sleep_ms`.
- A native `kama build triangle.kama --webgpu` (wgpu-native v29.0.1.1, GLFW 3.4) succeeds. It prints the two
  documented fnptr warnings, plus a new stray `clang: warning: '-x none' after last input file`. The smoke binary prints
  "WGPUInstance created", and `kama check` on the triangle passes for native and wasm without the SDK.
- **Nothing in CI reaches `tests/webgpu/*` or `examples/webgpu`.**
- The triangle leaks more than its five per-frame releases cover: the shader module, the `GetBindGroupLayout` ref, the
  adapter, the instance and the calloc'd `App`. It also hardcodes 512×512 BGRA8.
- webgpu.h ref-counts all 23 handle types (`Release`/`AddRef`). Releasing the last Device ref destroys it, so its
  children turn INVALID (validation errors) rather than dangling.
- The web leg is unprobed: there is no emcc on the host.

Sites:

- `kama_gpu.c`: `kama_gpu_surface` (~85) and `kama_gpu_pump` (~92). `kama_gpu.h`: the web bodies (~55–75).
- A new `lib/std/gpu/gpu.kama`, plus the `lib/kama.json` entry.
- kama.driver.cpp: the `needsGpu` link (~12985, ~13514), and the seam input order behind `-x none` (~13404).

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
   manifest entry; the `-x none` fix.
2. The remaining handles, with safe creation for exactly what the triangle uses.
3. The triangle on `std::gpu` with zero `wgpu*Release` calls; README, ENGINE_READINESS and SPEC.
4. `tools/check-gpu.sh`: `kama check` the example and fixtures, native and wasm, always (no SDK needed); build, link
   and run the smoke when `third_party/wgpu` (or emcc) exists, else SKIP. Plus an xfail for a frame used after
   `present()`.

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

- Native `kama_gpu_pump` calls `glfwPollEvents` with no callbacks registered, so only the close flag survives.
- The web pump is `return 1`, with no html5 listeners.
- No key, mouse, size or focus information exists anywhere.
- A pull-API prototype worked: a C ring queue, `type enum InputEvent`, `pollEvent() -> Optional<…>`, and
  `try cast<Key>(n)` falling back to `Unknown`. It needed a bound key to be matchable, which `0.9.634` made true.
- A downstream engine writes its own GLFW/html5 platform layer today, because the seam's window is unreachable and its
  events are discarded. This module must make that layer unnecessary: several windows, size and scale, and a native
  handle.

Sites:

- `kama_gpu.c`: `kama_gpu_pump` and `g_window`. `kama_gpu.h`: the web pump (~70).
- A new `kama_window.h`/`.c`.
- kama.driver.cpp: a `needsWindow` that links GLFW without wgpu.

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

  **The switch exists now** (checked 2026-10-08): the pinned CLI (0.26.11) has grammar-level `reserved` word
  sets, and the first set is reserved EVERYWHERE, while `reserved(name, rule)` overrides it per rule (the
  contextual words). That replaces both earlier candidates: an external scanner (the grammar has one now, for
  `give`/`copy`/`truncate`, a different job) and a negative lookahead over 78 spellings in `identifier`. The
  fix is that set, generated from `kama.l`'s keyword table so the two cannot drift.

  `null` belongs to the class too. The compiler's reserved-word note names it (`0.9.554`), but no xfail can
  pin that note until this lands: `int32 null` is a parse error to the compiler and clean to tree-sitter, so
  the fixture would break the parse-error parity `check-treesitter.sh` holds. Add `xfail/null_as_name` here.


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
