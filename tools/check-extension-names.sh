#!/bin/sh
# check-extension-names.sh — the VS Code name layer rewrites the DAP traffic correctly.
#
# `editor/vscode/names.js` is what makes LOCALS, the CALL STACK and the watch box read in kama rather
# than in the emitted C. No data formatter can reach those — they come from the debug info — so the
# extension edits the DAP messages in both directions, and the rules for doing that are the only part
# of the extension with real logic to get wrong:
#
#   * a `variables` response is used BOTH for a frame's locals and for the children of a value, and the
#     children have already been renamed by include/kama_lldb.py. Renaming both would strip a second
#     `k_` off a field the author really did call `k_x`.
#   * `evaluateName` must keep the C spelling, because it is the string sent back in an `evaluate`
#     request; rewriting it breaks Add-to-Watch.
#   * the REQUEST direction has to mangle, not demangle — the user types `near`, the adapter knows
#     `k_near`.
#
# The repo has no JS test harness at all, which is why names.js is deliberately free of any `vscode`
# import: it is plain functions over plain objects, and node can drive them here.
#
# ⚠️ WHAT THIS DOES NOT COVER, stated so nobody reads a green line as more than it is: whether VS Code
# honours an in-place edit made inside `onDidSendMessage`. An extension may only register a
# DebugAdapterDescriptorFactory for a debug type it defines, and `lldb` is CodeLLDB's, so a tracker is
# the only interception point available; a tracker is documented to OBSERVE. It works because the
# message object is not cloned before the tracker sees it. If that ever changes, names revert to the C
# spelling and this guard still passes. Only a live session can catch that — done 2026-09-20 on
# VS Code 1.125.1 + CodeLLDB 1.12.3 (macOS): the Call Stack read `probe`/`main` and Variables read
# `s = "hello"`, `pt = (x = 3, y = 4)`. Re-run that check by hand when the editor updates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v node >/dev/null 2>&1; then
    echo "check-extension-names: SKIP (no node on this host)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The extension must actually USE the module, or these rules are asserted about dead code. Same shape
# check-editors.sh uses to keep Zed's `"lsp"` spelling honest.
grep -q "require('./names')" "$ROOT/editor/vscode/extension.js" \
    || { echo "check-extension-names: FAIL — extension.js no longer requires ./names" >&2; exit 1; }
grep -q 'registerDebugAdapterTrackerFactory' "$ROOT/editor/vscode/extension.js" \
    || { echo "check-extension-names: FAIL — extension.js no longer registers a debug adapter tracker" >&2; exit 1; }
grep -q 'kamaSession' "$ROOT/editor/vscode/extension.js" \
    || { echo "check-extension-names: FAIL — the launch config no longer marks the session, so the tracker would rewrite every lldb session in the window" >&2; exit 1; }
# A PROJECT debugs from its own launch.json, and the ▶ button that runs one cannot invoke an extension
# command. Without this provider such a session gets no formatters and no name layer — raw C structs and
# mangled names, looking exactly like a working session. It is the difference between "the debugger
# shows kama" being true for a toy and true for a project.
grep -q 'registerDebugConfigurationProvider' "$ROOT/editor/vscode/extension.js" \
    || { echo "check-extension-names: FAIL — extension.js no longer registers a debug configuration provider, so a project's own launch.json gets neither formatters nor demangled names" >&2; exit 1; }
grep -q 'resolveDebugConfigurationWithSubstitutedVariables' "$ROOT/editor/vscode/extension.js" \
    || { echo "check-extension-names: FAIL — the provider must resolve WithSubstitutedVariables; \`program\` is still \${workspaceFolder}/… in the earlier hook and the symbol table cannot be read from it" >&2; exit 1; }
# The name layer must be gated a SECOND time on the binary's own symbols. Detection keys off a
# `kama.json` in the workspace, and a workspace can hold a kama project beside a Rust or C++ one — whose
# stack the lexical fallback would happily rewrite, since most C++ symbols contain `__`.
grep -q 'syms === null ? !mark.explicit : syms.length === 0' "$ROOT/editor/vscode/extension.js" \
    || { echo "check-extension-names: FAIL — the tracker no longer gates on the binary carrying kama symbols, so a foreign binary in a workspace that merely contains a kama.json would have its stack rewritten" >&2; exit 1; }

cat > "$tmp/t.js" <<'JS'
const assert = require('assert');
const { lexical, makeTable, makeTracker, isCompilerOwned, kamaOperand } = require(process.argv[2]);
let n = 0;
const eq = (got, want, what) => { assert.deepStrictEqual(got, want, what + ': got ' + JSON.stringify(got) + ', want ' + JSON.stringify(want)); n++; };

// --- the lexical fallback: what a name with no table entry still reads as -------------------------
eq(lexical('k_near'),           'near',       'a local loses the register');
eq(lexical('k_k_near'),         'k_near',     'EXACTLY one strip: the author really wrote k_near');
eq(lexical('k_Fapp__Pair'),     'Pair',       'a file-private scope comes off');
eq(lexical('k_Fkk__k_Box'),     'k_Box',      "a declaration's leaf keeps its own k_");
eq(lexical('kama_main'),        'main',       'the entry point');
eq(lexical('kama__print'),      'print',      'the prelude scope is implicit in source');
eq(lexical('std__collections__Map'), 'std::collections::Map', 'a module scope reads as a path');

// --- compiler-owned locals are machinery, except the one that is not ----------------------------
for (const t of ['kama_ret_0', 'kama_self', 'kama_match0', 'kama_msub1', 'kama_heap0'])
  eq(isCompilerOwned(t), true, t + ' is machinery');
eq(isCompilerOwned('kama_main'), false, 'kama_main is `main`, not machinery');
eq(isCompilerOwned('k_near'),    false, 'a user local is not machinery');

// --- the table wins over the fallback, which is the whole point of running the front end ---------
const table = makeTable({
  'k_Fapp__Pair_int32__make': 'Pair<int32>::make',
  'std__collections__DynamicArray_string_kama__GlobalAllocator': 'DynamicArray<string>',
});
eq(table.frame('k_Fapp__Pair_int32__make'), 'Pair<int32>::make', 'a frame renders its type arguments');
eq(table.type('std__collections__DynamicArray_string_kama__GlobalAllocator *'),
   'DynamicArray<string> *', 'a pointer keeps its decoration');
eq(table.name('k_near'), 'near', 'a name the table never saw falls back');

// --- the DAP rewriting ---------------------------------------------------------------------------
const tr = makeTracker(table);

// A `scopes` response is how we learn which refs hold FRAME LOCALS.
tr.onDidSendMessage({ type: 'response', command: 'scopes', body: { scopes: [{ variablesReference: 1000 }] } });

const stack = { type: 'response', command: 'stackTrace',
                body: { stackFrames: [{ name: 'k_Fapp__Pair_int32__make' }, { name: 'kama_main' }] } };
tr.onDidSendMessage(stack);
eq(stack.body.stackFrames.map((f) => f.name), ['Pair<int32>::make', 'main'], 'the call stack');

// A locals request against a scope ref -> renamed.
tr.onWillReceiveMessage({ type: 'request', command: 'variables', seq: 7, arguments: { variablesReference: 1000 } });
const locals = { type: 'response', command: 'variables', request_seq: 7,
  body: { variables: [
    { name: 'k_near',   type: 'int32_t',    evaluateName: 'k_near' },
    { name: 'k_k_x',    type: 'int32_t',    evaluateName: 'k_k_x' },
    { name: 'kama_ret_0', type: 'int32_t',  evaluateName: 'kama_ret_0' },
  ] } };
tr.onDidSendMessage(locals);
eq(locals.body.variables.map((v) => v.name), ['near', 'k_x'], 'locals renamed, machinery hidden');
eq(locals.body.variables[0].evaluateName, 'k_near', 'evaluateName keeps the C spelling');

// THE ONE THAT MATTERS: a `variables` response for the CHILDREN of a value is not a scope ref, and
// kama_lldb.py has already renamed those. Touching them would strip a second `k_`.
const kids = { type: 'response', command: 'variables', request_seq: 9,
               body: { variables: [{ name: 'k_x', type: 'int32_t' }] } };
tr.onDidSendMessage(kids);
eq(kids.body.variables.map((v) => v.name), ['k_x'], "a value's children are left alone (already renamed)");

// The request direction: the watch box is typed in kama.
const ev = { type: 'request', command: 'evaluate', seq: 11, arguments: { expression: 'near' } };
tr.onWillReceiveMessage(ev);
eq(ev.arguments.expression, 'k_near', 'a watch expression is mangled on the way in');
const ev2 = { type: 'request', command: 'evaluate', seq: 12, arguments: { expression: 'near.value' } };
tr.onWillReceiveMessage(ev2);
eq(ev2.arguments.expression, 'k_near.value', 'only the head is mangled; the path is synthetic children');

// A function breakpoint is typed by a human, so it is typed in kama.
const fb = { type: 'request', command: 'setFunctionBreakpoints', seq: 13,
             arguments: { breakpoints: [{ name: 'Pair<int32>::make' }] } };
tr.onWillReceiveMessage(fb);
eq(fb.arguments.breakpoints[0].name, 'k_Fapp__Pair_int32__make', 'a function breakpoint is mangled back');

// --- the two layers must MEET. kama_lldb.py renames frames LLDB-side, so a stackTrace response can
// --- arrive already lexically demangled; keyed only by the mangled spelling the table would miss it
// --- and the generic arguments would never render — the LLDB floor would become the ceiling.
eq(table.frame('Pair_int32::make'), 'Pair<int32>::make', 'a half-demangled frame still upgrades');
eq(table.mangle('Pair_int32::make'), 'k_Fapp__Pair_int32__make', '...and still resolves back for a breakpoint');

// --- WHICH SESSIONS ARE KAMA'S. Detection, not a key you have to know to add: a key you must add is
// --- itself a way for a project to debug with mangled names. `type: "lldb"` is shared with every
// --- Rust, C++ and Swift session in the window, so a false positive here reaches someone else's.
const os = require('os'), fsx = require('fs'), px = require('path');
const tmp = fsx.mkdtempSync(px.join(os.tmpdir(), 'kamaop-'));
fsx.mkdirSync(px.join(tmp, 'proj', 'build'), { recursive: true });
fsx.mkdirSync(px.join(tmp, 'plain', 'build'), { recursive: true });
fsx.writeFileSync(px.join(tmp, 'proj', 'kama.json'), '{"name":"p"}');
const folder = (n) => ({ uri: { fsPath: px.join(tmp, n) } });

eq(kamaOperand({ program: px.join(tmp, 'proj/build/app') }, folder('proj'), ''),
   px.join(tmp, 'proj', 'kama.json'),
   'a kama project is DETECTED with no key in the launch config');
eq(kamaOperand({ program: px.join(tmp, 'plain/build/app') }, folder('plain'), ''), '',
   'a project with no kama.json is left alone');
eq(kamaOperand({ kama: 'src/main.kama', program: px.join(tmp, 'plain/build/app') }, folder('plain'), ''),
   px.join(tmp, 'plain', 'src/main.kama'),
   'an explicit relative path still wins, for a manifest detection cannot find');
eq(kamaOperand({ program: px.join(tmp, 'plain/build/app') }, undefined, ''), '',
   'no workspace folder and no key: nothing to go on, so nothing is claimed');
eq(kamaOperand({ kama: true, program: px.join(tmp, 'plain/build/app') }, folder('plain'), '/x/open.kama'),
   '/x/open.kama',
   'with an explicit opt-in and nothing discoverable, the open file is the operand');
eq(kamaOperand({ program: px.join(tmp, 'plain/build/app') }, folder('plain'), '/x/open.kama'), '',
   '...but an open .kama alone never opts a foreign project in');
fsx.rmSync(tmp, { recursive: true, force: true });

// --- hostile input: a tracker that throws takes the debug session with it ------------------------
for (const bad of [undefined, null, 'a string, as some hosts send', 42, {}, { type: 'response' }]) {
  tr.onWillReceiveMessage(bad);
  tr.onDidSendMessage(bad);
}
n++;
console.log('  ok: ' + n + ' assertions');
JS

if ! node "$tmp/t.js" "$ROOT/editor/vscode/names.js" > "$tmp/out" 2>&1; then
    echo "check-extension-names: FAIL — the name layer does not behave as specified:" >&2
    sed 's/^/  /' "$tmp/out" >&2
    exit 1
fi
cat "$tmp/out"
echo "check-extension-names: OK (locals, call stack, watch and breakpoints; a value's children untouched)"
