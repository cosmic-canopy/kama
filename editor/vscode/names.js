// names.js — the kama name layer for the debugger, as pure functions over DAP messages.
//
// What is left after LLDB has done everything it can. `include/kama_lldb.py` owns values, and its
// `frame-format` hook owns frame names — so a plain `lldb` and every non-VS-Code editor already get
// readable stacks. Two things it cannot reach, and they are what this file is for:
//
//   * a LOCAL's name. `frame variable` prints the DWARF name and LLDB has no hook to rewrite it.
//   * the GENERIC ARGUMENTS in a frame name. The LLDB hook is lexical — `Pair_int32::make` — because
//     rendering `Pair<int32>::make` needs the resolved program, which only `kama demangle` has. So
//     this layer upgrades what that one produced rather than replacing it, and the table is indexed
//     by BOTH spellings so the two meet.
//
// Both require sitting between VS Code and the debug adapter and editing the DAP traffic.
//
// ⚠️ NO `vscode` IMPORT, DELIBERATELY. Everything here is a plain function over plain objects so it can
// be run by node and asserted — see tools/check-extension-names.sh. The extension has no test harness of
// any kind, and this is the one part of it with real logic to get wrong.

// ---------------------------------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------------------------------

// A mangled name is turned back by `kama demangle`, which runs the compiler's front end — the only
// thing that can render `std__collections__DynamicArray_int32_kama__GlobalAllocator` as
// `DynamicArray<int32>`, because the argument spellings and the dropped default live in the resolved
// program, not in the text of the name.
//
// ⚠️ IT MUST BE BUILT UP FRONT, not asked per stop. `onDidSendMessage` is synchronous — there is
// nowhere to await — while `createDebugAdapterTracker` may return a Thenable that VS Code awaits. So
// the whole table is resolved in that window and every later rewrite is a Map lookup. This is a
// refinement of the roadmap's "batched over stdin, one process per debug session": the batching is
// front-loaded into session start, because the place the names are needed cannot wait.
//
// `lexical` is the fallback for a name the table never saw, and it exists because the mangling is
// reversible BY CONSTRUCTION (SPEC § *C names*): strip exactly one `k_`, drop a scope prefix. It cannot
// render generic arguments — that is what the table is for — but it is never wrong about the register,
// and it means a name the analysis missed still reads better than raw C.
function lexical(name) {
  if (typeof name !== 'string' || !name) return name;
  let s = name;
  // `kama_main` is the entry point under the compiler's own register.
  if (s === 'kama_main') return 'main';
  // A file-private scope is `k_F<stem>__`; a module scope is `<a>__<b>__`. Both are prefixes no user
  // can spell in source, so they come off; what is left may itself be `k_`-prefixed and must not be
  // stripped again — a type the author called `k_Box` emits `k_Fkk__k_Box`.
  const fileScoped = /^k_F[A-Za-z0-9_]+?__(.+)$/.exec(s);
  if (fileScoped) return fileScoped[1].replace(/__/g, '::');
  if (s.startsWith('kama__')) return s.slice(6).replace(/__/g, '::');
  if (s.startsWith('k_')) return s.slice(2);
  return s.replace(/__/g, '::');
}

// Build the lookup used for the whole session. `answers` maps a mangled spelling to what `kama
// demangle` said; anything absent falls through to `lexical`.
function makeTable(answers) {
  const map = answers instanceof Map ? answers : new Map(Object.entries(answers || {}));
  // ⚠️ ALSO index by the LEXICAL form, because the name may already be half-demangled by the time it
  // gets here. `include/kama_lldb.py` renames frames LLDB-side, which is what gives a plain `lldb` and
  // every non-VS-Code editor a readable call stack — so a `stackTrace` response can arrive carrying
  // `Pair_int32::make` rather than `k_Fapp__Pair_int32__make`. Keyed only by the mangled spelling, the
  // lookup would miss and the generic arguments would never be rendered: the LLDB layer's floor would
  // silently become the ceiling. The two layers have to meet on a common key.
  const alias = new Map();
  for (const [mangled, display] of map) {
    const lex = lexical(mangled);
    if (lex !== display && !map.has(lex)) alias.set(lex, display);
  }
  // display -> mangled, built on first use. Only the request direction needs it, and most sessions
  // never type a watch expression or a function breakpoint at all.
  let reverse = null;
  const one = (name) => {
    if (typeof name !== 'string' || !name) return name;
    const hit = map.get(name);
    if (hit !== undefined) return hit;
    const lex = alias.get(name);
    if (lex !== undefined) return lex;
    return lexical(name);
  };
  return {
    name: one,
    frame: one,
    // A type as the debugger spells it may be a pointer (`kama__Optional_string *`) or otherwise
    // decorated; rewrite the identifier and leave the decoration alone.
    type: (t) => (typeof t === 'string' ? t.replace(/[A-Za-z_][A-Za-z0-9_]*/g, (tok) => one(tok)) : t),
    // The REQUEST direction. A user types `near.value` in the watch box; the adapter must be asked
    // about `k_near`. Only the head is mangled: the rest of the path is navigated through synthetic
    // children, which kama_lldb.py already presents under kama names.
    mangle: (expr) => {
      if (typeof expr !== 'string' || !expr) return expr;
      // WHOLE-STRING first. A function breakpoint is a rendered name in its entirety —
      // `Pair<int32>::make` — and it is not an identifier at all, so the head rule below would make
      // `k_Pair<int32>::make` out of it. A watch expression is the other shape and falls through.
      if (reverse === null) {
        reverse = new Map([...map].map(([k, v]) => [v, k]));
        // A user typing a function breakpoint types what they SEE, which may be either layer's
        // rendering, so both resolve back to the one symbol the adapter knows.
        for (const [lex, display] of alias) if (!reverse.has(lex)) reverse.set(lex, reverse.get(display));
      }
      const whole = reverse.get(expr);
      if (whole !== undefined) return whole;
      const m = /^([A-Za-z_][A-Za-z0-9_]*)/.exec(expr);
      if (!m) return expr;
      const head = m[1];
      const hit = reverse.get(head);
      if (hit !== undefined) return hit + expr.slice(head.length);
      // Not a name the analysis knew: it is a local, and a local is `k_` plus what the author wrote.
      return 'k_' + head + expr.slice(head.length);
    },
  };
}

// A compiler-owned local is machinery, not a binding the author wrote: `kama_ret_0`, `kama_self`,
// `kama_match0`, `kama_msub1`, `kama_heap0`. Showing them in a variables pane is noise, and worse, it
// invites someone to conclude their program has variables it does not.
//
// ⚠️ `kama_main` is NOT machinery — it is `main`. Filtering on the register alone would hide the one
// frame every stack ends at.
function isCompilerOwned(name) {
  return typeof name === 'string' && name.startsWith('kama_') && name !== 'kama_main';
}

// ---------------------------------------------------------------------------------------------------
// The DAP rewriting
// ---------------------------------------------------------------------------------------------------

// Which messages carry a name, and what must NOT be touched:
//
//   stackTrace response   body.stackFrames[].name       the CALL STACK
//   variables response    body.variables[].name/.type   LOCALS
//   variables response    body.variables[].evaluateName LEAVE IT. It is the string sent back in an
//                                                       `evaluate` request, so keeping the C spelling
//                                                       is what keeps Add-to-Watch and Copy-Value
//                                                       working without a second translation.
//   scopes response       body.scopes[].variablesReference   nothing to rename, but this is how we
//                                                       learn which refs hold FRAME LOCALS
//   evaluate request      arguments.expression          the watch box, mangled on the way in
//
// ⚠️ WHY THE SCOPE-REF BOOKKEEPING. A `variables` response is used for two different things: a frame's
// locals, and the children of a value. The children of a value are produced by kama_lldb.py, which has
// ALREADY renamed them. Renaming everything here would strip a second `k_` off a field the author
// really did call `k_x` (emitted `k_k_x`, rendered `k_x` by the formatter, and `x` by a second pass).
// So only responses to requests that asked about a ref named in a `scopes` response are touched.
function makeTracker(table, opts) {
  const hideCompilerLocals = !opts || opts.hideCompilerLocals !== false;
  const scopeRefs = new Set();
  const pending = new Set();

  return {
    onWillReceiveMessage(m) {
      // Some hosts hand the tracker a STRING rather than a parsed object. Nothing below is safe on
      // one, and a throw here takes the debug session with it.
      if (!m || typeof m !== 'object') return;
      if (m.type !== 'request' || !m.arguments) return;
      if (m.command === 'variables' && scopeRefs.has(m.arguments.variablesReference)) pending.add(m.seq);
      else if (m.command === 'evaluate') m.arguments.expression = table.mangle(m.arguments.expression);
      else if (m.command === 'setFunctionBreakpoints' && Array.isArray(m.arguments.breakpoints)) {
        // A function breakpoint is typed by a human, so it is typed in kama. `br set -n probe` against
        // a kama binary resolves to nothing today; the symbol is `k_Fapp__probe`.
        for (const b of m.arguments.breakpoints) if (b && b.name) b.name = table.mangle(b.name);
      }
    },

    onDidSendMessage(m) {
      if (!m || typeof m !== 'object' || m.type !== 'response' || !m.body) return;
      if (m.command === 'scopes') {
        for (const s of m.body.scopes || []) scopeRefs.add(s.variablesReference);
      } else if (m.command === 'stackTrace') {
        for (const f of m.body.stackFrames || []) if (f && f.name) f.name = table.frame(f.name);
      } else if (m.command === 'variables' && pending.delete(m.request_seq)) {
        let vars = m.body.variables || [];
        if (hideCompilerLocals) vars = vars.filter((v) => !v || !isCompilerOwned(v.name));
        for (const v of vars) {
          if (!v) continue;
          if (v.name) v.name = table.name(v.name);      // evaluateName is deliberately left alone
          if (v.type) v.type = table.type(v.type);
        }
        m.body.variables = vars;
      } else if (m.command === 'evaluate' && m.body.type) {
        m.body.type = table.type(m.body.type);
      }
    },
  };
}

module.exports = { lexical, makeTable, makeTracker, isCompilerOwned };
