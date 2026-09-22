; Syntax highlighting for kama in ZED'S capture vocabulary.
;
; This is deliberately NOT a copy of tree-sitter-kama/queries/highlights.scm. Zed's theme system knows a
; flat ~44-entry set — @keyword, @function, @type, @variable, @property, @variant, … — and has no
; @keyword.control.conditional, no @function.method and no @variable.other.member. An unknown capture is
; not an error in Zed; it simply produces no colour, so a Helix-vocabulary file would load cleanly and
; render half the buffer grey. tools/check-treesitter.sh compiles both files against the grammar and checks
; that they reference the same set of node types, so a construct cannot be coloured in one editor and
; silently forgotten in the other.
;
; The two precedence rules measured for Helix are assumed to hold here too and are worth preserving either
; way: least-specific first, and never capture a wrapper node when the thing you mean is the identifier
; inside it. See the header of the canonical file for how those were established.

; ── generic fallback, FIRST ────────────────────────────────────────────────────────────────────────────
(identifier) @variable

; ── punctuation and operators ──────────────────────────────────────────────────────────────────────────
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" "." ":"] @punctuation.delimiter
[":=" "?" "::" "#"] @operator
(binary_expression operator: _ @operator)
(unary_expression operator: _ @operator)
(update_expression operator: _ @operator)
(assignment_expression operator: _ @operator)
(overloadable_operator) @operator

; ── keywords ───────────────────────────────────────────────────────────────────────────────────────────
; `expose`, `hardware` and `this` are each the whole content of their own named rule, so no bare anonymous
; token exists for them and naming one is a query ERROR. They are captured as nodes below.
[
  "import" "export" "as" "type" "enum" "extends" "implements" "for"
  "friend" "operator" "ctor" "fn" "fnptr" "extern" "comptime" "when"
  "in" "unsafe" "asm" "static" "const" "default" "slot" "file"
  "if" "else" "match" "case"
  "while" "do" "foreach" "parallel_for" "parallel_spawn"
  "break" "continue" "return"
  "spawn" "scope" "borrow"
  "new" "try" "cast" "bitcast" "truncate" "sizeof" "alignof"
] @keyword

(intrinsic_name) @function.builtin   ; addr drop panic assert debugAssert — reserved call-site intrinsics

(modifier) @keyword
(function_modifier) @keyword
(hardware) @keyword

; Scoped to the parent node: a bare ["give" "copy"] pattern also matches the anonymous token inside
; (method_name), which colours `fn Point copy()` as a keyword.
(handoff_expression operator: _ @keyword)
(handoff_default) @keyword

(this_expression) @variable.special
"base" @variable.special

; ── comments ───────────────────────────────────────────────────────────────────────────────────────────
(line_comment) @comment
(block_comment) @comment
; kama.l DROPS `^[ \t]*#.*` silently, so treating it as a comment is what the compiler actually does.
(preproc_line) @comment

; ── literals ───────────────────────────────────────────────────────────────────────────────────────────
(integer_literal) @number
(float_literal) @number
(boolean_literal) @boolean
(null_literal) @constant
(char_literal) @string
(string_literal) @string
(verbatim_string_literal) @string
(tagged_string_literal) @string
(escape_sequence) @string.escape
(quote_escape) @string.escape
(interpolation "${" @punctuation.special)
(interpolation "}" @punctuation.special)
(format_spec) @string.special
(string_tag (identifier) @function)

; ── types ──────────────────────────────────────────────────────────────────────────────────────────────
(primitive_type) @type.builtin
(void_type) @type.builtin
(type_name name: (identifier) @type)
(type_declaration_head name: (identifier) @type)
(type_parameter name: (identifier) @type)

; ── calls ──────────────────────────────────────────────────────────────────────────────────────────────
(call_expression function: (identifier) @function)
(call_expression function: (field_expression field: (identifier) @function))
(call_expression function: (scoped_identifier name: (identifier) @function))
(call_expression function: (turbofish_name name: (identifier) @function))
(call_expression function: (turbofish_type_member member: (identifier) @function))
(call_expression function: (turbofish_member name: (identifier) @function))
(argument name: (identifier) @variable.parameter)

; ── declarations, most specific and therefore last ─────────────────────────────────────────────────────
(function_declaration name: (identifier) @function)
(extern_declaration name: (identifier) @function)
(extern_const_declaration name: (identifier) @constant)
(fnptr_declaration name: (identifier) @function)
(method_declaration name: (method_name) @function)
(method_declaration name: (method_name (identifier) @function))
(constructor_declaration name: (identifier) @constructor)
(constructor_declaration name: (method_name) @constructor)
(constructor_declaration name: (method_name (identifier) @constructor))
(destructor_declaration name: (identifier) @constructor)
(enum_member name: (identifier) @variant)
(match_pattern name: (identifier) @variant)
(parameter name: (identifier) @variable.parameter)
(field_declaration (variable_declarator name: (identifier) @property))
(attribute "@" @attribute)
(attribute name: (identifier) @attribute)

; ── the contextual kind words, last of all ─────────────────────────────────────────────────────────────
; `value` / `resource` / `view` / `contract` are ordinary IDENTIFIERS in kama.l, so only a dedicated node
; can colour them — and it must target the identifier INSIDE type_kind, not type_kind itself.
(type_kind (identifier) @keyword)
(kind_name (identifier) @keyword)
