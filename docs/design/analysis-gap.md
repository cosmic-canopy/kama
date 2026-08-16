# The analysis gap — findings ⑩ and ⑪, and the test-infra holes (in-flight design)

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> **Read this before the ROADMAP row it replaces.** Every claim below was **probed against the built
> compiler on 2026-08-15**, with the command recorded. The row this brief supersedes described ⑩ as
> *"an uninstantiated generic body gets no analysis"*. That is true, and it is **not the root cause** —
> probing found a larger one underneath it. The view-window campaign shipped a row asserting it closed
> findings ④⑤⑥ and closed none of them, because a plan's "closes" column got copied forward as a result;
> this file shows its work so that cannot happen a third time.

## Status

| | |
|---|---|
| **Closed by the view window** | ④⑤⑥ (the window rule, the freeze, `ref`/`out` view params, view-root aliasing) and ⑦ (`foreach` is a window; `mods` bumped in `growTo`). `1655e8a`…`19fc999` |
| **OPEN — this document** | ⑩ (no expression type-checking), ⑪ (silent narrowing), the test-infra holes |

## What the probes established

### 1. ⑩ is not about generics. **kama never type-checks an initializer at all.**

This is the finding that reframes the row. The ROADMAP described ⑩ as an *uninstantiated generic* problem.
It is not — the same hole is wide open in a plain, non-generic `main`:

```kama
fn int32 main() { int32 x = "not an int"; return 0; }
```

```sh
kama check c_plain.kama          # -> "OK (1 unit analyzed)"    <-- accepted
kama build c_plain.kama -o /tmp/cp
# -> clang: error: initializing 'int32_t' with an expression of incompatible type 'kama_string'
```

**The type error is caught by clang, against generated C the author never wrote — never by kama.** So:

- `kama check` accepts it, which means **the editor shows a broken file as clean**. That is the exact
  disagreement `run_tests.sh`'s analysis-agreement leg exists to catch, and the leg is real — it simply has
  **no fixture** of this shape. (The same pattern produced the `foreach`-rvalue defect closed in `25175fa`:
  a genuine hole, guarded in principle, unfixtured in practice.)
- The diagnostic a user actually gets names a C type (`kama_string`) and a C line number.

**This is the largest single correctness item left before the tag**, and it is a *missing subsystem*, not a
missing rule: kama has no expression type-checker. Scope it as one.

### 2. A never-instantiated generic body gets **no analysis whatsoever**

```kama
fn void wrong<T>(ref DynamicArray<T> d) {
    View<T> v = d.view();          // window-rule violation
    int32 x = "not an int";        // type error
    undefinedFunction(a: 1);       // unresolved name
    d.nonexistentMethod();         // unresolved method
}
fn int32 main() { return 0; }
```

```sh
kama check g_never.kama          # -> OK
kama build g_never.kama -o /tmp/gn   # -> "kama: built /tmp/gn"     <-- FOUR errors, builds clean
```

Not just types: **name resolution, method resolution and the view rules are all deferred to
instantiation.** This is the half the ROADMAP named, and it is the one that hits package authors —
ship `check`-green, consumers get the errors — but note it is a *second* defect, not the cause of §1.

⚠️ **Do not assume the two share a fix.** §1 needs a type-checker; §2 needs the analysis passes to run over
a template body with its parameters treated as opaque. §2 without §1 still leaves the plain case open;
§1 without §2 still leaves the generic body unanalyzed.

### 3. ⑪ is two problems, and only one of them is mechanical

```kama
int8 a = cast<int8>(300);   // CONSTANT, provably out of range  -> 44, silently
int32 big = 300;
int8 b = cast<int8>(big);   // runtime value                    -> 44, silently
```

Both truncate; the program exits 88. The **constant** case is statically provable and the compiler already
has the folder (`constValue`) — rejecting it is mechanical and clearly right, and there is precedent:
`xfail/constgen_oob` rejects a constant out-of-range array index while a dynamic one traps at runtime.

The **runtime** case is a **design question and needs a decision, not an implementation**:
- leave it (C/Rust behaviour — Rust's `as` truncates silently and offers `try_into` for checked), or
- trap at runtime like the bounds check does, or
- add a checked spelling and leave `cast` as the unchecked one.

⚠️ Decide this with the user before writing code. GOALS' *one way to do a thing* cuts against adding a
second cast spelling; the bounds-check precedent cuts toward trapping.

### 4. The test-infra holes are what let all of the above hide

- `tests/trap/` is **skipped** under `KAMA_SAN`, `KAMA_WASM`, and on Windows/MSYS2 (`run_tests.sh:553`) —
  UBSan intercepts the trap and node's abort codes differ.
- **No leg runs MSan.** Per [[dev-infra]], MSan-origins catches what ASan and wasm both miss.
- **An `xfail` fixture never links, so it never reaches ASan.** This is why the view-window campaign's
  acceptance test had to be a probe ledger rather than a green suite — ④⑤⑥ survived all three legs and 34
  guards. Any campaign about *rejection* needs the same discipline.

## Milestones (suggested — re-scope after §1's design is settled)

1. **⑪-constant** — reject a provably out-of-range constant narrowing cast. Mechanical, self-contained,
   `xfail/cast_narrow_const`. Do this first: it is the one piece needing no design.
2. **⑪-runtime** — take the decision above, then implement it (or record it as a declared non-goal).
3. **⑩a — the expression type-checker.** The big one. Start by scoping *where* it lives: kama is a
   tree-walking C emitter with no separate type pass, so this is new machinery, not a new rule in an
   existing one. **Write the `xfail` first** — `int32 x = "not an int"` must be rejected by `kama check`
   as well as `kama build`.
4. **⑩b — analyse uninstantiated generic bodies** with type parameters opaque.
5. **Test-infra** — an MSan leg, and `tests/trap/` running somewhere it currently does not.

## Traps — each already cost something in this repo

| trap | consequence |
|---|---|
| **A doc is not evidence.** This row's own description of ⑩ was wrong about the root cause | a brief that starts from the ROADMAP would build a generics fix and leave the plain case open |
| **A green `./dev matrix` proves little for a REJECTION campaign** — an `xfail` never links, so it never reaches ASan, and the analysis-agreement leg only covers fixtures that exist | write the `xfail` first; the acceptance test is a probe ledger |
| **`kama check` must reject whatever `kama build` rejects** (`run_tests.sh:645`) | a rule implemented only on the emit path shows a broken file as clean in the editor |
| the prelude is compiled INTO the binary | `./dev build` after any `prelude/global.kama` edit |
| a breaking rule and its corpus migration must land in ONE commit | `run_tests.sh` fails any fixture whose stderr matches `/warning/i`, and `unsupported()` prints `warning:` |

## Verification

- `./dev matrix > /tmp/m.log 2>&1; tail -5 /tmp/m.log`, then grep the **same** file. Once per milestone.
- Every fixture must be rejected by **`kama check`** as well as `kama build`.
- The probes in §1–§3 are the acceptance test: each must stop compiling (or start being diagnosed by kama
  rather than by clang), and the resulting diagnostics belong in the commit message.
