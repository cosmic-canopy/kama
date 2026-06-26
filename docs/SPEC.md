# cstar language specification (overview)

This is a semantics overview. The **grammar is authoritative** — see
[grammar.bnf](grammar.bnf) (generated from `cstar.y`). Executable examples live in
[`../tests/`](../tests/) (`*.cstar` with a `.expect` exit code). Status flags below: ✅ implemented,
🚧 planned.

## Model

cstar compiles to **portable C**. No garbage collector — object lifetimes are deterministic (RAII).
Calls use **named arguments** (no positional). The philosophy: one way to do a thing, explicit over
implicit (see [../GOALS.md](../GOALS.md)).

## Types ✅

| cstar | C |
|---|---|
| `int8 int16 int32/int int64` | `int8_t … int64_t` |
| `uint8 uint16 uint32 uint64` | `uint8_t … uint64_t` |
| `float32` / `float64`/`double` | `float` / `double` |
| `bool` | `bool` |
| `string` | `cstar_string` (runtime; literals borrow) |
| `void` | `void` |
| user `class` | `struct` (value semantics) |

No raw arrays **by design** — collections are generic library types (`List<T>`/`Array<T>`), 🚧 post-v1.

## Functions ✅

```cstar
int add(int a, int b) { return a + b; }
int main() { return add(b: 20, a: 10); }   // named args; reordered to declared order
```
`ref`/`out` parameters pass by pointer: `void set(out int dst) { dst = 42; }` … `set(dst: ref x);`.
`extern Ret name(params);` declares a function provided by C (no body emitted) — the FFI seam. ✅

## Control flow ✅

`if/else`, `while`, `do/while`, `for`, `switch` (C#-style, no fall-through), `break`, `continue`,
`return`; full operator set (`+ - * / %`, bitwise, shifts, comparisons, `&& || !`, ternary `?:`),
assignment ops (`= += …`), `++`/`--`, casts.

## Classes ✅

```cstar
class Counter {
    int value;
    Counter(int start) { value = start; }   // constructor
    void add(int n) { value = value + n; }   // method (implicit self)
    int get() { return value; }
}
Counter c = new Counter(start: 40);   // constructs in place
c.add(n: 2);
```
Fields, methods (take an implicit `self`), one constructor, field initializers (run in the ctor),
`this.field`, `obj.method(args)`. Lowers to a `struct` + `Counter__method(Counter* self, …)` functions.

## RAII / destructors ✅

A `~Type()` destructor runs deterministically at scope exit, in reverse construction order, on every
path (block end, early `return`, `break`/`continue`). Destructible fields are destroyed in reverse
declaration order. No GC; allocation/deallocation is predictable.

## Inheritance & virtual dispatch ✅ (M6)

```cstar
class Shape { int describe() { return this.area(); }  virtual int area() { return 0; } }
class Circle extends Shape { override int area() { return 42; } }
```
Single inheritance (`extends`), base embedded by value (upcast is offset-0), base ctor via `: base(...)`,
`base.m()` for non-virtual upcalls. `virtual`/`override` methods dispatch through a vtable. `interface`/
`implements` are 🚧 M6b.

## Enums ✅

```cstar
enum Color { Red, Green = 5, Blue }   // Red=0, Green=5, Blue=6
Color c = Color.Blue;
```
Lowers to a C `enum` (members mangled `Color_Red`…). Enum values are integers — usable in `switch`,
comparisons, and `cast`.

## Namespaces & modules 🚧

`namespace a.b;` and `using` parse; full multi-file modules are planned (M7+).

## Building & debugging ✅

```sh
cstar build app.cstar                 # native debug (-g, breakpoints in .cstar via #line)
cstar build app.cstar --release       # optimized, stripped, NDEBUG
cstar build app.cstar --target wasm   # browser: .html + .js + .wasm
```
Debug builds are breakpoint-debuggable in an IDE (locals + call stack map back to `.cstar`).

## Reserved/runtime

Generated C reserves `__`-prefixed identifiers (`__base`, `__vptr`, `__ret_N`) and `Type__member`
mangling. The runtime ([../cstar_runtime.h](../cstar_runtime.h)) provides `cstar_string` and a
`cstar_trace`/`cstar_trace_get` hook used by tests.
