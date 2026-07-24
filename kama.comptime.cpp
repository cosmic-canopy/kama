// kama.comptime.cpp — const-eval 6b-3: the compile-time function (`comptime fn`) subsystem.
//
// A `comptime fn` is a bounded, pure function the compiler RUNS at compile time to bake a `static const`
// scalar or table (a CRC/gamma/trig LUT) into the emitted C — zero runtime cost, sits in .rodata/flash.
// It is comptime-ONLY: never emitted as a C symbol, callable only from a comptime context.
//
// This translation unit is the interpreter — a sibling of the emitter that produces *values*, not C text.
// It is kept as its own TU (defining CEmitter methods) so the eval kernel stays modular: a future
// scripting/REPL backend (GOALS §6/§10) can reuse the walk + value model under a permissive policy, while
// purity/budget are the compile-time policy layered on top here.
//
// Stage 1 lands only the registration-adjacent helper (runtime-call rejection). The CTValue interpreter,
// step budget, and comptime-const evaluation land in Stage 2+.

#include "kama.cemit.h"
#include "kama.ast.h"

// Does `name` (with an optional scope qualifier) resolve to a registered `comptime fn`? Used at the
// call-emit sites to reject a runtime-position call with a diagnostic that points at the `comptime`
// constant form. Mirrors the common resolveFunc search (same-module, `using`, prelude) over _comptimeFns;
// an explicit `mod::f()` qualifier that misses simply falls through to the generic unknown-fn diagnostic.
bool CEmitter::isComptimeFnName(const std::string& name, SharedStringList /*qualifier*/, std::string& outKey) const
{
    auto hit = [&](const std::string& k) -> bool {
        auto i = _comptimeFns.find(k);
        if (i != _comptimeFns.end()) { outKey = k; return true; }
        return false;
    };
    if (hit(qualify(name))) return true;                                  // same-module `f()`
    for (auto& u : _nsCtx.usings) if (hit(u + "__" + name)) return true;  // imported via `using`
    return hit(name);                                                     // prelude / global namespace
}
