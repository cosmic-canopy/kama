# C names — a kama name reaches C in a namespace no header can rewrite (KR-67)

**Status:** measured and recommended 2026-09-17 on the Windows box at `0.9.379`. **Not started.** Waiting on the
maintainer's ruling on the four decisions below. Delete this doc when KR-67 ships, as the maintenance rule for
`docs/design/` says.

## Picking this up

Everything a fresh session needs is here and in the KR-67 row. Nothing depends on an assistant's memory or a
scratch directory.

1. `git fetch && git rebase origin/dev`, `./dev build`.
2. Read **Decisions** and confirm the maintainer has ruled. Do not start the sweep on the recommendation alone: D1
   extends a rule `src/kama.l` wrote down (reserve, don't rename), and a fork against a written plan goes to the
   maintainer first.
3. Land the red-first fixture (**Plan** step 1) and see it fail on Windows. It should also fail on the Linux box
   (`_LP64`, `errno`).
4. Then the sweep, in the order in **Plan**.

On Windows use the UCRT64 login-shell form from `docs/platforms/windows.md`. The gate is `./dev test` then
`./dev check`. This change touches emission on every target, so the maintainer's `./dev matrix` on the Linux box
(san, wasm) and a macOS build are part of it too.

## What is true today (measured, `0.9.379`, Windows UCRT64)

kama emits some names into C as the user wrote them. The C preprocessor is not scoped, so any object-like macro
from any included header rewrites such a name, and any function-like macro rewrites it when a `(` follows.

**Which positions, probed one each with a program importing `std::fs` (so `kama_os.h` is in the TU):**

| position | probe | result |
|---|---|---|
| struct field | `public int32 near;` | ❌ `expected identifier` |
| parameter | `fn int32 near(int32 far)` | ❌ |
| local | `int32 far = 3;` | ❌ |
| enum payload field | `Circle(int32 near)` | ❌ |
| contract vtable slot | `fn int32 min(int32 a);` called through the contract | ❌ `(p).vtbl->min(…)` hits `min(a,b)` |
| fn-pointer local | `Op max = id; max(x: 0)` | ❌ `too few arguments … function-like macro` |
| module `static` | `static int32 near;` | ✅ prefixed |
| type name | `type value DrawText` (`DrawText` is a winuser.h macro) | ✅ prefixed `_F<file>__DrawText` |
| enum case | `CopyFile` (a winbase.h macro) | ✅ prefixed |
| free function / method | — | ✅ prefixed |
| control, no `std::` import | `int32 far = 3;` | ✅ (no `kama_os.h` in the TU) |

**How big the hostile set is.** `clang -dM -E` over one module TU of that program: **21,748 macros, 17,332 not
starting with `_`**. The lowercase object-like ones are `near far pascal cdecl environ errno s_addr s6_addr h_addr
h_errno isascii toascii in_addr6 …` (48 in all). The lowercase function-like ones include `min max offsetof
alloca va_start timerclear`, plus 723 PascalCase macros (`CopyFile`, `DrawText`, …). One generated `<name>.gen.h`
includes every `extern` header, and every module's C includes it, so importing `std::fs` ANYWHERE puts all of this
in front of EVERY module's names. The set also moves with the SDK version and with each user's own
`extern "<vendor.h>"`.

**It is not Windows-only.** `_LP64` is predefined for `x86_64-linux-gnu` (checked here with `--target`), and kama
identifiers may start with `_`. `errno` is a macro everywhere, and POSIX `<sys/stat.h>` makes `st_mtime` one. The
POSIX lowercase set is not measured yet: take it on the Linux and macOS boxes with the same `-dM` command.

**Found this way:** `<iphlpapi.h>` defining `interface` broke every `std::net` program on Windows at `0.9.376` (fixed
`0.9.379` by not including it). That is one instance; the class is this doc.

## Decisions — the recommendation, for the maintainer

### D1. Mechanism: every name kama OWNS reaches C prefixed, and the declared C surface keeps its spelling

**Recommended.** Types, functions, statics and enum cases already live in a kama-owned C namespace (`_F…`,
`std__…`), which is why they never broke. Extend that one rule to the remaining positions: fields, variant payload
fields and union members, parameters, locals and bindings, and contract/vtable slot names. The positions that
DECLARE C keep the C spelling, because C code on the other side depends on it: `extern fn` names, `type extern
value` fields (must match the header), `type expose value`/`expose enum` fields and values, and `expose fn` names
(the host header).

Why this over the alternatives, against GOALS:
- **Closes the class by construction** (the house rule: by construction, not per site). It covers every target,
  every SDK version and every user FFI header, including ones kama has never seen. Nothing else on the list does.
- **Source stays portable** ("runs anywhere C runs"). A program that builds on Linux builds on Windows, with no
  per-platform banned-word list for an author who cannot see the other platform's headers.
- **One rule** (GOALS #4): "kama-owned names are prefixed; declared C is not", instead of a keyword table plus a
  macro list plus per-position exceptions.
- **Zero runtime cost** (the performance invariant) and no extra build step (GOALS #2).
- **It answers the three reasons `kama.l` gave for reserving C keywords instead of renaming them**, because those
  were reasons against *selective* renaming. A user's own `k_switch` cannot clash once every name is prefixed (it
  becomes `k_k_switch`). The `extern`/`expose` names are exactly the exempt set. And the C name never reaches a
  diagnostic, hover or query answer, since those are keyed by the AST (confirmed by the inventory below).

Rejected, with the measured reason:
- **`#undef` a list after the SDK includes.** Partial: `errno`, `environ`, `s_addr`, `min`/`max` cannot be undefined,
  because the seam and user FFI use them. **Unsafe:** once `errno` is undefined, a later runtime macro that expands
  to `errno` binds to a user local of that name, a silent miscompile. It also drifts with SDK versions and never
  sees a user's own headers.
- **Reserve the spellings in the lexer, as for C keywords.** Unbounded (17K on one target), different per target
  and SDK, and it would refuse on Linux a name that is only a macro on Windows.
- **Header isolation** (move the OS seam out of the shared `gen.h` into its own TU). Covers only kama's own headers,
  not a user's `extern "<windows.h>"` or vendor header, and gives up inlining the thin syscall wrappers. It may still
  be worth doing for COMPILE SPEED (see **Adjacent**), but it is not this fix.
- **Detect collisions with `clang -dM -E` and rename only those.** Keeps debugger names for the common case, but the
  C name then depends on the target and the header set (implicit, against GOALS #5). It costs a preprocessor pass
  per build, needs the macro set before emission, and would still rename silently.

### D2. Spelling: the prefix `k_`

**Recommended: `k_<name>`** (`near` → `k_near`, `_LP64` → `k__LP64`).
- It must not start with `_` + capital or `__`. That is C's implementation namespace, where system headers live:
  `_LP64` is a real predefined macro, so an `_L` prefix would collide for a local named `P64`.
- **Measured on Windows: 0 of 21,748 macros start with `k_`.** Nothing kama emits starts with `k_` today (0 hits in
  the emitter's literals or the runtime headers). It is short, so a debugger reads `k_near`.
- **A suffix was measured worse:** six COM macros end in `_` (`STDAPI_`, `STDMETHODIMP_`, …), so a field `STDAPI`
  would collide.
- **The residual, stated:** a macro in some user FFI header that literally starts with `k_`. kama cannot see every
  header, so the guard below holds the line for kama's OWN header set on every box, and SPEC says so.

### D3. `expose fn` parameter names in the host header

**Recommended: keep the kama spelling in the published header** and prefix them in the implementation C. Parameter
names in a C prototype are not ABI and may differ from the definition, and the header is documentation for the host
author, compiled in the HOST's macro environment, which kama cannot see. A collision there is part of the declared
C surface, like an `expose` field. Record that residual in SPEC *Exposing to a host*.

### D4. Keep reserving C's keywords in the lexer

**Recommended: keep `c_reserved` as is.** After D1 only the declared-C positions still need it, so relaxing it for
kama-owned positions would become possible and source-compatible at any time. It is **genuinely optional** (nobody
needs a local named `switch`), and one lexer rule for every position is simpler than two. Rewrite the `kama.l`
comment to say this, so the old "reserve, don't rename" rationale does not read as contradicted.

**Consequences to accept (not decisions):** a native debugger shows `k_near` for kama-owned locals and fields.
DWARF carries the C name, and a pretty-printer cannot rename a variable. Note it in the debugging docs beside KR-32.
Clang errors only appear on compiler bugs, and would name `k_…`.

## Inventory (read at `0.9.376`; find by function NAME, the numbers drift)

No helper exists. Every site writes the raw AST string (`FieldInfo::name`, `VariantCase::name`, `ParamSig::name`,
`*p->identifier->value`, `InterfaceMethod::name`, `VSlot::name`), and the same string keys analysis maps
(`_localTypes`, `_moveState["x.f"]`, `recordDestructibleLocal`, `_refParams`). **So the kama KEY stays as it is, and
only the C WRITE goes through the helper.** Rough site counts in `src/kama.cemit.cpp`:
- **Fields ~30:** `emitStruct`, `emitMemberAccess`, the bare field in a method, `base.field`, field initializers,
  `@generate(of)`, and the derived bodies (serialize/equals/hash/format/deserialize, object-graph serde).
- **Variant payload ~15:** `emitVariantStruct`, `emitVariantConstruction`, match bindings, the dtor, derived bodies.
- **Parameters ~10:** `paramListC` (prototype and definition), `vtableSlotSig`/`ifaceSlotSig` (thunk casts),
  `scalarSlotThunk`, the fn-ptr typedef, `writeHostHeader` (D3), the identifier arm.
- **Locals 50–80:** `emitDeclarator` in `emitStatement`, foreach, match bindings, the identifier arm, and the 22
  `recordDestructible*` calls that build dtor C from the name.
- **Vtable slots ~12:** `emitVtableType`/`emitVtableInstance`, `emitInterfaceTypes`, `emitClassInterfaceVtables`,
  the intrinsic vtable, contract and virtual dispatch, fn-ptr field calls.
- **Literals naming kama-DECLARED members, ~290 lines:** `Optional`/`Result` (`.u.Some.value`, `.u.Err.error`, ~58),
  `FieldKey` payloads (8), the Serializer/Deserializer slot names (142 `vtbl->X` literals over 23 names), the
  `Owned`/`Shared`/`Weak` fields `p`/`c`/`alloc` (~30; ⚠️ `alloc` is ALSO a runtime-macro member, so check each hit),
  `Template`'s `_parts`/`_nparts`/`_holes`/`_nholes`, and `View`'s `.data`/`.len`.
- **Not renamed** (C-runtime structs and compiler-owned names): `kama_string` `.data/.len/.cap`, `kama_ctrl`, the
  `obj/vtbl/ctrl/alloc/objsize` members of the `KAMA_*_IFACE_TYPE` macros, `tag`, `u`, `__vptr`, `__dtor`, `__size`,
  `__align`, `__type`, capture fields `c<i>`, `InlineArray` `.v`, and temporaries such as `__ret_0`.
- **Wire and display names must stay the kama spelling:** serde `wire = f.serName.empty() ? f.name : f.serName`,
  variant payload wire names (`f.name`, with no `serName` fallback), variant tags, and `@generate(Formattable)` text.
- **Outside the emitter:** no `include/*.h` reads an emitted field by name. `tests/expose_value.d/csrc/host.c` reads
  expose fields (exempt). `tools/check-slot.sh` and `tools/check-ecs-zero-dispatch.sh` grep emitted C for names and
  need updating.
- **Diagnostics:** clang stderr passes through unmapped (`kama.driver.cpp`), `#line` fixes only file and line,
  `demangleForDisplay` covers types only, LSP/query are AST-keyed, and `check-diag-drift.sh` checks line numbers.

**A useful property:** a missed site is LOUD, not silent. A struct member renamed at its declaration but read raw
somewhere else (or the reverse) is a clang error. The one silent failure would be a wire or display string that got
prefixed, and the serde and format fixtures compare bytes, so those catch it.

## Plan

1. **Red first: `tests/c_macro_names.kama`**, portable, with a hostile name in every position, each a macro on at
   least one target: field `near`, parameter `far`, local `errno`, local `_LP64`, payload field `s6_addr`, contract
   member `min`, fn-pointer local `max`, field `st_mtime`, local `environ`. It imports `std::fs`, `std::net` and
   `std::process`, so the seam's headers are in the TU, and returns a checksum. Red today on Windows (every position)
   and on Linux (`_LP64`, `errno`, `st_mtime`). Each box's green run is the witness, as with `check-long-path.sh`.
2. **The helper:** one function for a kama-owned C identifier (`k_` + name) and one for a member of a given owner
   (raw when the owner is `isExternStruct`/`isExposeStruct`). Named constants for the prelude members the emitter
   hardcodes, so the 290 literals read `"." + kField("value")` and stay greppable.
3. **Sweep by position:** fields and payloads (with the prelude literals), then vtable slots (with the 142 serde slot
   literals), then parameters, then locals and bindings. Build and run `./dev test` between positions: every miss
   is a clang error in some fixture.
4. **`writeHostHeader`:** raw parameter names (D3). Fields of an expose type stay raw on both sides.
5. **Tools:** update `check-slot.sh` and `check-ecs-zero-dispatch.sh`. Add **`tools/check-c-names.sh`**, which
   `clang -dM -E`s the full shipped header set (`kama_runtime.h`, `kama_os.h`, every `include/*.h` a module can
   `extern`) for this host's target and FAILS if any macro starts with `k_`, so D2's premise is checked on every box
   rather than assumed.
6. **Docs:** SPEC (a short *C names* rule under the C-interop section, plus D3's residual under *Exposing to a
   host*), the `kama.l` comment (D4), `docs/platforms/windows.md` (the class is closed), and the debugger note.
7. **`VERSION` bump.** Gate: Windows `./dev test` + `./dev check`, then the maintainer's `./dev matrix` on Linux and
   a macOS build, and take the POSIX `-dM` lowercase set there to record beside the Windows one.

**Size: L.** Mechanical but wide (~200 sites plus ~290 literals), self-checking by the suite, and no runtime change.

## Adjacent — measured here, not part of KR-67

**Every module TU preprocesses the whole OS header set.** Preprocessing `#include "kama_os.h"` took ~263 ms against
~121 ms for `kama_runtime.h` alone on this VM (five runs each, including process start). So importing `std::fs`
adds ~140 ms to EVERY module's C compile. That is a QEMU + x64-emulation figure, so read only the ratio. Moving the
OS seam behind plain prototypes (bodies in one TU, or kept inline only where it measurably matters) could be a real
compile-time win (GOALS #2) on every platform. It would also shrink the macro surface, though D1 is what makes names
safe. Worth its own row if the maintainer wants it measured on a native box. Do not fold it into KR-67.
