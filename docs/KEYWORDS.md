# cstar keyword audit (M19)

> **Keywords added since the audit**: `fn` (M20 — every function/method declaration), `fnptr` (M21 — an
> explicit function-pointer type), and the `::` scope-resolution operator (M20b). All three are implemented.

Status of all **59 reserved keywords**, established by exercising each through `cstar transpile`/`build`
(the emitter dispatches on AST nodes, not tokens, so this is empirical). Legend:

- ✅ **works** — parses, lowers, behaves correctly
- ⚠️ **partial** — works in some forms, missing others
- 🪦 **dead** — parses but is silently dropped (a no-op)
- 🔒 **unenforced** — parses, but its *guarantee* is not checked
- ❌ **unsupported** — reserved; using it is a hard error
- 🐞 **bug** — see notes

## Status table

| Keyword(s) | Status | Notes |
|---|---|---|
| `bool` `int8/16/32/64` `uint8/16/32/64` `float32` `float64` `void` `string` | ✅ | core types |
| `int` | ✅ | alias → `int32` |
| `double` | ✅ | alias → `float64` (C `double`) |
| `class` `interface` `enum` `namespace` `operator` | ✅ | |
| `extends` `implements` `new` `this` `base` `ref` `out` | ✅ | (`base(...)`/`base.m()` need **named** args) |
| `virtual` `override` | ✅ / 🐞 | dispatch works **only if the derived class has a constructor** — see Bug 1 |
| `if` `else` `switch` `case` `default` `do` `while` `for` `foreach` `in` `break` `continue` `return` | ✅ | |
| `using` `true` `false` `null` `cast` `unsafe` `extern` | ✅ | |
| `static` | ⚠️ | a static method is callable via an **instance** (`c.foo()`), but class-level `C::foo()` is `unsupported`, and `static` adds no real static semantics (the method still takes `self`) |
| `volatile` | ❌ | **reserved** for the embedded/MMIO scope (ISR↔loop shared flags, peripheral registers) — using it is a clear error, not a silent no-op; implemented when cstar targets embedded |
| `export` | ❌ | **reserved** for the cstar→host boundary (WASM module exports, scripting host interface) — clear error on use, not a silent no-op |
| `public` `private` `protected` | 🔒 | parsed, but **no access enforcement** — a `private` field is readable from outside the class |
| `friend` | 🔒 | `friend(list)` parses, but enforces nothing (access control itself is unenforced) |
| `abstract` | 🔒 | method dispatch works, but instantiating an abstract class (`new AbstractType()`) is **not prevented** |
| `final` | 🔒 | **not enforced** — `extends` a `final` class and `override` of a `final` method are both allowed |
| `const` | 🚧 | **const locals (M24a) + const methods (M24b)** work. A `const` local is deeply immutable (no reassign, no write *through* it, no `++`/`--`); a `const fn` method is non-mutating (`this` is const inside it) and is the *only* kind callable on a const receiver. const **params/fields** + `const T*` (FFI) land in M24c/M24d |

## Bugs found

**Bug 1 — polymorphic class without an explicit constructor crashes (SIGBUS).**
`class D extends B { override int32 f() {…} }` with `new D()` (no `D` ctor) segfaults on the first virtual
call. Root cause: `__vptr` is assigned **only inside constructor bodies** ([cstar.cemit.cpp:2077-2079](../cstar.cemit.cpp#L2077)),
and `new` skips the ctor when `hasCtor == false` ([cstar.cemit.cpp:609-615](../cstar.cemit.cpp#L609)), so the
vtable pointer is left uninitialized. Adding any constructor to `D` fixes it. Proper fix: synthesize a
default constructor for any class with a vtable (sets `__vptr`, chains the base ctor). Not a keyword issue;
tracked as a focused codegen follow-up.

## Dispositions

| Item | Decision | Status |
|---|---|---|
| `const` | implement in **M24** (const-correctness); stays a hard error until then | deferred |
| `volatile` | **reserved** — genuinely needed for the **embedded/Arduino** scope the user works in (ISR↔main-loop shared flags and memory-mapped peripheral registers, where `volatile` is the *correct* tool on a single-core MCU — not the multicore-atomics misconception). Using it today is a clear error; implement properly (emit C `volatile`) when cstar targets embedded — alongside the bigger embedded needs (globals/statics, ISR attributes, no-heap mode, MCU toolchains). | ✅ done (reserved + errors on use) |
| `export` | **kept reserved** — it has a real future role at the cstar→host boundary (WASM module exports for the browser engine; the scripting host interface), which is distinct from in-language `public`/`private`. Using it today is a **clear hard error**, never a silent no-op. | ✅ done (errors on use) |
| `public` `private` `protected` `friend` `abstract` `final` | enforcement is the **Access-Control milestone** (backlog) — they parse today but their guarantees aren't checked; documented here so the gap is explicit, not silent. The milestone also evaluates: *abstract*/*virtual* ≤ protected, and granular/mandatory `friend` member lists. | deferred (documented) |
| `static` | full static (class-level `C::foo()`, no implicit `self`) deferred; pairs naturally with the `::` operator (M20). Callable via an instance today. | deferred (documented) |
| **Bug 1** (vtable init) | **fixed** — `buildVtables` now synthesizes a default constructor for any polymorphic class lacking one (sets `__vptr`, chains the base ctor); a synth ctor that can't supply required base args is a clear error. Regression: `tests/vtable_default_ctor`. | ✅ done |

No keyword is left as a *silent* no-op after this audit: `volatile` is removed, `export` is a clear error
pending its WASM/host implementation, the access-control words are documented with a committed milestone,
`const` is a clear error pending M24, and the vtable crash is fixed.
