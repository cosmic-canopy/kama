; Syntax highlighting for kama — the CANONICAL query, in the Helix/nvim-treesitter capture vocabulary.
; Zed has its own, flatter vocabulary; see ../../editor/zed/languages/kama/highlights.scm.
;
; ⚠️ TWO PRECEDENCE RULES GOVERN THIS FILE, AND BOTH WERE MEASURED, NOT READ. Helix and Neovim document
; pattern precedence inconsistently and Helix reversed its own order in 2025, so this was settled by
; rendering a buffer in a real Helix 25.07.1 through a pty and reading the SGR colours back
; (tools/helix-render.py, which is kept precisely so the next person can re-measure instead of guess):
;
;   1. ON THE SAME NODE, THE LAST MATCHING PATTERN WINS. So this file is written LEAST-SPECIFIC FIRST and
;      the generic `(identifier) @variable` is the very FIRST rule. Written last, it silently clobbered all
;      fifteen specific captures and every identifier rendered as plain default foreground.
;
;   2. A CAPTURE ON A DEEPER NODE BEATS ONE ON ITS ANCESTOR, whatever the order. This is the subtler of the
;      two and it defeats rule 1 entirely: `(type_kind) @keyword.storage.type` never applied, because
;      type_kind CONTAINS an identifier and `(identifier) @variable` sits on that deeper node. Every
;      wrapper-node capture here therefore targets the inner identifier — `(type_kind (identifier) @...)`.
;      The tell was that `fn T copy()` coloured correctly while `fn T area()` did not: `copy` is an
;      anonymous token with no identifier beneath it to lose to.
;
; Neither failure errors, and neither is visible to `tree-sitter query`, which reports every match rather
; than the winner. The only instrument that sees them is a real editor. Re-run:
;
;     python3 tools/helix-render.py <file.kama> --expect='1:value==4:fn' --expect='3:value!=1:value'
;
; A THIRD rule, which ordering cannot fix: never let a bare anonymous token compete with a named node.
; `copy`/`give` may name a method (kama.y:980), so `["give" "copy"] @keyword.operator` painted
; `fn Point copy()` keyword-coloured wherever it sat; the handoff captures are scoped to their parents.
; Constructs needing this were given dedicated nodes in grammar.js: type_kind, kind_name, method_name,
; string_tag, format_spec, preproc_line.

; ── generic fallbacks, FIRST so everything below overrides them ────────────────────────────────────────
(identifier) @variable

; ── punctuation and operators ──────────────────────────────────────────────────────────────────────────
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" "." ":"] @punctuation.delimiter
[":=" "?" "::"] @operator
(binary_expression operator: _ @operator)
(unary_expression operator: _ @operator)
(update_expression operator: _ @operator)
(assignment_expression operator: _ @operator)
(overloadable_operator) @operator

; ── keywords ───────────────────────────────────────────────────────────────────────────────────────────
; NOTE `expose`, `hardware` and `this` are absent here: each is the entire content of its own named rule,
; so tree-sitter never emits a bare anonymous token for it and naming one is a query ERROR, not a no-op.
; They are captured as nodes below.
[
  "namespace" "import" "export" "as" "type" "enum" "extends" "implements" "for"
  "friend" "operator" "ctor" "fn" "fnptr" "extern" "comptime" "when"
  "in" "unsafe" "asm" "static" "const" "default" "slot"
] @keyword

["if" "else" "match" "case"] @keyword.control.conditional
["while" "do" "for" "foreach" "parallel_for"] @keyword.control.repeat
["break" "continue" "return"] @keyword.control.return
["spawn" "scope" "borrow"] @keyword.control
["new" "try"] @keyword.operator
["cast" "bitcast" "sizeof" "alignof"] @keyword.operator

(modifier) @keyword.storage.modifier
(function_modifier) @keyword.storage.modifier
(hardware) @keyword.storage.modifier

; `give`/`copy` are scoped to their PARENT nodes. A bare ["give" "copy"] token pattern also matches the
; anonymous token inside (method_name), which is how `fn Point copy()` came back keyword-coloured.
(handoff_expression operator: _ @keyword.operator)
(handoff_default) @keyword.operator

(this_expression) @variable.builtin
"base" @variable.builtin

; ── comments ───────────────────────────────────────────────────────────────────────────────────────────
(line_comment) @comment.line
(block_comment) @comment.block
; kama.l DROPS `^[ \t]*#.*` silently, so colouring it as a comment is what the compiler actually does.
(preproc_line) @comment.line

; ── literals ───────────────────────────────────────────────────────────────────────────────────────────
(integer_literal) @constant.numeric.integer
(float_literal) @constant.numeric.float
(boolean_literal) @constant.builtin.boolean
(null_literal) @constant.builtin
(char_literal) @constant.character
(string_literal) @string
(verbatim_string_literal) @string
(tagged_string_literal) @string
(escape_sequence) @constant.character.escape
(quote_escape) @constant.character.escape
(interpolation "${" @punctuation.special)
(interpolation "}" @punctuation.special)
(format_spec) @string.special
(string_tag (identifier) @function.macro)

; ── types ──────────────────────────────────────────────────────────────────────────────────────────────
(primitive_type) @type.builtin
(void_type) @type.builtin
(type_name name: (identifier) @type)
(type_declaration_head name: (identifier) @type)
(type_parameter name: (identifier) @type.parameter)

; ── calls ──────────────────────────────────────────────────────────────────────────────────────────────
(call_expression function: (identifier) @function)
(call_expression function: (field_expression field: (identifier) @function.method))
(call_expression function: (scoped_identifier name: (identifier) @function))
(call_expression function: (turbofish_name name: (identifier) @function))
(call_expression function: (turbofish_type_member member: (identifier) @function.method))
(call_expression function: (turbofish_member name: (identifier) @function.method))
(argument name: (identifier) @variable.parameter)

; ── declarations, most specific and therefore LAST ─────────────────────────────────────────────────────
(function_declaration name: (identifier) @function)
(extern_declaration name: (identifier) @function)
(fnptr_declaration name: (identifier) @function)
; A wrapper-node capture is invisible when the wrapper contains an identifier — see the ordering note at
; the top. Capture BOTH: the inner identifier for `fn T area()`, and the node itself for `fn T copy()`,
; where the child is an anonymous token and there is no identifier to target.
(method_declaration name: (method_name) @function.method)
(method_declaration name: (method_name (identifier) @function.method))
(constructor_declaration name: (identifier) @constructor)
(constructor_declaration name: (method_name) @constructor)
(constructor_declaration name: (method_name (identifier) @constructor))
(destructor_declaration name: (identifier) @constructor)
(enum_member name: (identifier) @type.enum.variant)
(match_pattern name: (identifier) @type.enum.variant)
(parameter name: (identifier) @variable.parameter)
(field_declaration (variable_declarator name: (identifier) @variable.other.member))
(attribute "@" @attribute)
(attribute name: (identifier) @attribute)

; ── the contextual kind words, LAST of all ─────────────────────────────────────────────────────────────
; `value` / `resource` / `view` / `contract` are ordinary IDENTIFIERS in kama.l, so nothing but a dedicated
; node can colour them — in the TextMate grammar the equivalent rule was present, correct and unreachable.
; Being last is what makes them survive `(identifier) @variable`.
(type_kind (identifier) @keyword.storage.type)
(kind_name (identifier) @keyword.storage.type)
