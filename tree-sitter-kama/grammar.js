/**
 * tree-sitter grammar for the kama programming language.
 *
 * SOURCE OF TRUTH IS `../kama.l` AND `../kama.y`, NOT THIS FILE. This is the third grammar for kama
 * (after the compiler's own Flex/Bison front end and the TextMate grammar in
 * ../editor/vscode/syntaxes/kama.tmLanguage.json), and the only reason it is allowed to exist is that
 * `../tools/check-treesitter.sh` keeps it honest:
 *
 *   - every keyword in kama.l's table must appear here,
 *   - the committed src/ must regenerate byte-identically from this file,
 *   - the corpus tests in test/corpus/ must pass, and
 *   - every .kama file the COMPILER accepts must parse here with zero ERROR nodes, while every file the
 *     compiler rejects with a parse/lexical error must produce one.
 *
 * That last oracle is the one that matters. A grammar that merely looks right colours real code wrongly.
 *
 * ── Design notes, each earned ──────────────────────────────────────────────────────────────────────
 *
 * CONTEXTUAL KIND WORDS. `value` / `resource` / `view` / `contract` are NOT keywords in kama — they are
 * ordinary identifiers that the emitter interprets positionally (kama.y `marked_type_declaration`). Writing
 * them as literal strings here would (a) promote them to keywords via `word:` and break `int32 value = 1;`,
 * and (b) hardcode a set kama.y deliberately leaves open. They get a dedicated NODE (`type_kind`) instead,
 * so a query can colour them without depending on pattern order. This is the direct fix for the TextMate
 * dead-rule defect, where the equivalent rule was present, correct, and unreachable.
 *
 * NAMED ARGUMENTS ONLY. kama has no positional arguments anywhere (kama.y `argument`). `f(1)` is a syntax
 * error; it must be `f(x: 1)`. There is deliberately no positional alternative in `argument` below.
 *
 * NO `->`. The return type precedes the name: `fn int32 add(...)`.
 *
 * `>>` INSIDE GENERICS needs no external scanner. tree-sitter builds its lexer per LR state from that
 * state's valid symbols, so in a state where only `,` and `>` can follow a type, `>>` is not a candidate
 * and maximal munch yields `>`. kama makes this easier than C++/Rust for a reason worth keeping: its
 * `_statement_expression` is restricted (kama.y:499) so `a < b;` is NOT a legal statement, which means a
 * leading `IDENT <` at statement position can only be a declaration. Mirror that restriction — it is
 * load-bearing. test/corpus/generics.txt falsifies this.
 *
 * PRECEDENCE IS FLATTENED. kama.y expresses precedence as a 13-level cascade of nonterminals
 * (multiplicative_expression -> additive_expression -> ...) with no %left/%right declarations at all.
 * Mirroring that here would wrap every expression in 13 single-child nodes and make every query and every
 * corpus expectation unreadable. `prec.left`/`prec.right` reproduce the same associativity exactly; the
 * PREC table below names the kama.y nonterminal each level replaces so a future precedence change has an
 * obvious landing site.
 */

const PREC = {
  // each entry names the kama.y nonterminal it replaces (kama.y:614-885)
  assignment: 1, // assignment                 (right-assoc)
  handoff: 1, // expression: GIVE / COPY
  ternary: 2, // conditional_expression        (right-assoc)
  or: 3, // conditional_or_expression
  and: 4, // conditional_and_expression
  bitor: 5, // inclusive_or_expression
  bitxor: 6, // exclusive_or_expression
  bitand: 7, // and_expression
  equality: 8, // equality_expression
  relational: 9, // relational_expression
  shift: 10, // shift_expression
  additive: 11, // additive_expression
  multiplicative: 12, // multiplicative_expression
  unary: 13, // unary_expression
  postfix: 14, // postfix_expression
  call: 15, // invocation_expression
  member: 16, // member_access / element_access
};

/** kama.y `integral_type` + `floating_point_type` + BOOL / CHAR / STRING. */
const PRIMITIVE_TYPES = [
  'int', 'int8', 'int16', 'int32', 'int64',
  'uint8', 'uint16', 'uint32', 'uint64',
  'double', 'float32', 'float64',
  'bool', 'char', 'string',
];

/** kama.y `modifier` — the MEMBER modifier set. Deliberately disjoint from `expose` (free functions only)
 *  and from the dedicated `const`/`hardware` slots; collapsing them loses tests/xfail/expose_on_method. */
const MODIFIERS = [
  'abstract', 'extern', 'override', 'private', 'protected',
  'public', 'final', 'static', 'default', 'virtual', 'immutable',
  'unsafe',
];

/** kama.y `overloadable_operator`. */
const OVERLOADABLE_OPERATORS = [
  '+', '-', '!', '~', '++', '--', '*', '/', '%',
  '&', '|', '^', '<<', '>>', '==', '!=', '>', '<', '>=', '<=',
];

const commaSep1 = (rule) => seq(rule, repeat(seq(',', rule)));
const commaSep = (rule) => optional(commaSep1(rule));

module.exports = grammar({
  name: 'kama',

  // Keyword extraction. Without this, `newType` lexes as `new` + `Type` and the keyword/identifier tie
  // becomes order-dependent rather than longest-match.
  word: ($) => $.identifier,

  extras: ($) => [
    /\s/,
    $.line_comment,
    $.block_comment,
    // kama.l DROPS `^[ \t]*#.*` silently (reserved for #region/#endregion). tree-sitter regexes have no
    // line anchors, but `#` appears in no other lexer rule, so an unanchored token is equivalent in
    // practice — and being a node rather than a skipped pattern lets a query colour it like a comment,
    // which is what the compiler effectively does with it.
    $.preproc_line,
  ],

  conflicts: ($) => [
    // `import a::b::{X};` — after `a`, a `::` may continue the path or open the symbol brace.
    [$.import_path],
    // The declaration-vs-expression fork every C-family grammar has: at statement position `Foo` may open
    // a local declaration (`Foo x = 1;`) or an expression (`Foo.bar();`), and `Foo <` may open type
    // arguments (`Map<K,V> m;`) or be a comparison. GLR explores both and the wrong branch dies at the
    // next token — kama makes that cheap because `_statement_expression` is restricted, so a comparison
    // can never actually complete a statement. See the `>>` note in the header.
    [$.type_name, $._expression],
    [$.scoped_identifier, $.type_name],
  ],

  rules: {
    // ── Compilation unit ────────────────────────────────────────────────────────────────────────────
    // kama.y:14 — the header order is FIXED and not interleavable. An `import` after a `fn`, a top-level
    // statement, or a top-level `const` is a syntax error, and staying faithful here is what lets the
    // whole-corpus oracle have teeth.
    source_file: ($) =>
      seq(
        optional($.namespace_declaration),
        repeat($.import_declaration),
        optional($.export_manifest),
        repeat($._top_level_declaration),
      ),

    namespace_declaration: ($) =>
      seq('namespace', field('name', $._qualified_name), ';'),

    // `import a::b;` / `import a::b as m;` / `import a::b::{X, Y as Z};`. The path gets its own rule (and
    // its own declared conflict) because deciding whether a `::` continues the path or opens the symbol
    // brace needs two tokens of lookahead, which LR(1) does not have.
    import_declaration: ($) =>
      seq(
        'import',
        field('path', $.import_path),
        optional(
          choice(
            seq('as', field('alias', $.identifier)),
            seq('::', '{', commaSep1($.import_symbol), '}'),
          ),
        ),
        ';',
      ),

    import_path: ($) => seq($.identifier, repeat(seq('::', $.identifier))),

    import_symbol: ($) =>
      seq(
        field('name', $.identifier),
        optional(seq('as', field('alias', $.identifier))),
      ),

    export_manifest: ($) => seq('export', '{', commaSep1($.identifier), '}', ';'),

    _top_level_declaration: ($) =>
      choice(
        $.function_declaration,
        $.extern_declaration,
        $.fnptr_declaration,
        $.type_declaration,
        $.intrinsic_declaration,
        $.enum_declaration,
        $.module_variable_declaration,
        $.comptime_assert_statement,
      ),

    // ── Type declarations ───────────────────────────────────────────────────────────────────────────
    // kama.y:248. `type <modifiers> <kind> Name ...` where <kind> is an ORDINARY IDENTIFIER. See the
    // header note; `type_kind` exists so a query never has to rely on pattern ordering to colour it.
    type_declaration: ($) =>
      seq(
        optional($.attribute_list),
        'type',
        repeat($.modifier),
        field('kind', $.type_kind),
        field('name', $.type_declaration_head),
        optional($.for_kinds),
        optional($.class_base),
        field('body', $.class_body),
        optional(';'),
      ),

    // `value` / `resource` / `view` / `contract` / `intrinsic` — contextual, never reserved.
    type_kind: ($) => $.identifier,

    // `type intrinsic <int8, int16, …> implements C { … <int8> { … } }` — contract conformance for a
    // PRIMITIVE. It diverges from `type_declaration` one token after the kind word: `<` rather than a
    // NAME. The target list is primitives only, which is why it uses `primitive_type` and not `type_name`
    // — the same reason kama.y uses `simple_type` there.
    intrinsic_declaration: ($) =>
      seq(
        optional($.attribute_list),
        'type',
        repeat($.modifier),
        field('kind', $.type_kind),
        field('targets', $.intrinsic_targets),
        optional($.class_base),
        field('body', $.intrinsic_body),
        optional(';'),
      ),

    intrinsic_targets: ($) => seq('<', commaSep1($.primitive_type), '>'),

    // A member is either shared by every target, or inside a `<…> { … }` SECTION overriding it for the
    // targets it names. A section can only begin with `<`, which no class member can.
    intrinsic_body: ($) =>
      seq('{', repeat(choice($._class_member, $.intrinsic_section)), '}'),

    intrinsic_section: ($) =>
      seq(field('targets', $.intrinsic_targets), '{', repeat($._class_member), '}'),

    // kama.y:735 — `for value, view` on a contract: a COMMA LIST, no `|` alternative. Kind words again,
    // contextual.
    for_kinds: ($) => seq('for', commaSep1($.kind_name)),
    kind_name: ($) => $.identifier,

    // kama.y:277 — the DECLARATION-site head, whose params can carry bounds. Distinct from the USE-site
    // `type_arguments`, which cannot.
    type_declaration_head: ($) =>
      seq(field('name', $.identifier), optional($.type_parameters)),

    type_parameters: ($) => seq('<', commaSep1($.type_parameter), '>'),

    type_parameter: ($) =>
      choice(
        // `T is This` — an IDENTITY constraint pinning the parameter to the implementing type. Kept out
        // of `bound_list` because a bound holds a CONTRACT. `is` is anonymous here rather than an
        // `$.identifier`: the compiler rejects any other word in this slot as a PARSE error, so a
        // permissive rule would make tree-sitter accept what the compiler will not. Listed first so
        // `T is …` cannot reduce as name-then-default.
        seq(
          field('name', $.identifier),
          'is',
          field('pin', $.type_name),
        ),
        seq(
          field('name', $.identifier),
          optional(seq(':', field('bounds', $.bound_list))),
          optional($._type_parameter_default),
        ),
        // `const N: int32` — a compile-time VALUE parameter.
        seq(
          'const',
          field('name', $.identifier),
          ':',
          field('type', $.primitive_type),
          optional($._type_parameter_default),
        ),
      ),

    _type_parameter_default: ($) => seq('=', $._type_or_value_argument),

    bound_list: ($) => seq($.type_name, repeat(seq('+', $.type_name))),

    class_base: ($) =>
      choice(
        seq('extends', field('superclass', $.type_name)),
        seq('implements', commaSep1($.implements_entry)),
        seq(
          'extends',
          field('superclass', $.type_name),
          'implements',
          commaSep1($.implements_entry),
        ),
      ),

    // kama.y:906 — an entry may carry a contract PARAMETER, `implements Copyable(bare: give)`.
    implements_entry: ($) =>
      seq(
        $.type_name,
        optional(
          seq('(', field('name', $.identifier), ':', $.handoff_default, ')'),
        ),
        optional($.when_clause),
      ),

    handoff_default: ($) => choice('give', 'copy'),

    // kama.y:917 — SQUARE brackets, mandatory. `default` is a legal structural bound.
    when_clause: ($) => seq('when', '[', commaSep1($.when_condition), ']'),

    when_condition: ($) =>
      seq(
        field('parameter', $.identifier),
        ':',
        field('bound', choice($.type_name, 'default')),
      ),

    // ── Enums ───────────────────────────────────────────────────────────────────────────────────────
    // `type enum Name<T> : IntType implements C { A, B(payload…); …members… }`. An enum is a type kind
    // like any other, so it takes the `type` marker and a `class_base`. The marker is MANDATORY (GOALS #3c:
    // every declaration is `type <kind> Name`); bare `enum X` is a parse error in kama.y too.
    enum_declaration: ($) =>
      seq(
        optional($.attribute_list),
        'type',
        repeat($.modifier),
        'enum',
        field('name', $.type_declaration_head),
        optional(seq(':', field('underlying', $.primitive_type))),
        optional(field('base', $.class_base)),
        field('body', $.enum_body),
        optional(';'),
      ),

    // A trailing comma is allowed. A `;` after the variants opens an ordinary class-member list — the
    // methods that satisfy the declared contract. The separator is MANDATORY: without it a bare `Foo`
    // variant and a `Foo bar;` field are indistinguishable at one token of lookahead (kama.y says the
    // same, which is why the four alternatives there are spelled out rather than folded into an `_opt`).
    enum_body: ($) =>
      seq(
        '{',
        optional(
          seq(
            commaSep1($.enum_member),
            optional(','),
            optional(seq(';', repeat($._class_member))),
          ),
        ),
        '}',
      ),

    enum_member: ($) =>
      seq(
        field('name', $.identifier),
        optional(
          choice(
            seq('=', field('value', $._expression)),
            // A tagged-union payload is a PARAMETER LIST — typed, named fields — not bare types.
            field('payload', $.parameter_list),
          ),
        ),
      ),

    // ── Module variables ────────────────────────────────────────────────────────────────────────────
    module_variable_declaration: ($) =>
      seq(
        optional($.attribute_list),
        choice(
          seq('static', optional($.hardware), field('type', $._type)),
          seq('comptime', field('type', $._type)),
        ),
        commaSep1($.variable_declarator),
        ';',
      ),

    hardware: ($) => 'hardware',

    // ── Functions ───────────────────────────────────────────────────────────────────────────────────
    // kama.y:329. NOTE the return type sits BETWEEN `fn` and the name — kama has no `->`.
    function_declaration: ($) =>
      seq(
        optional($.attribute_list),
        optional($.function_modifier),
        optional('unsafe'),
        optional('comptime'),
        'fn',
        field('return_type', $._function_return_type),
        field('name', $.identifier),
        optional($.type_parameters),
        field('parameters', $.parameter_list),
        field('body', $.block),
      ),

    // `expose` only — free functions. NOT part of the member `modifier` set.
    function_modifier: ($) => 'expose',

    _function_return_type: ($) =>
      choice($._type, $.void_type, $.reference_return_type),

    void_type: ($) => 'void',
    reference_return_type: ($) => seq('ref', $._type),

    // `extern "<stdio.h>";` and `extern fn T name(...);`
    extern_declaration: ($) =>
      choice(
        seq('extern', field('header', $.string_literal), ';'),
        seq(
          'extern',
          'fn',
          field('return_type', $._function_return_type),
          field('name', $.identifier),
          field('parameters', $.parameter_list),
          ';',
        ),
      ),

    fnptr_declaration: ($) =>
      seq(
        'fnptr',
        field('return_type', $._function_return_type),
        field('name', $.identifier),
        field('parameters', $.parameter_list),
        ';',
      ),

    parameter_list: ($) => seq('(', commaSep($.parameter), ')'),

    // kama.y:384 — no default values, no varargs.
    parameter: ($) =>
      seq(
        optional('const'),
        optional($.hardware),
        optional(field('mode', choice('ref', 'out'))),
        field('type', $._type),
        field('name', $.identifier),
      ),

    // ── Class body ──────────────────────────────────────────────────────────────────────────────────
    class_body: ($) => seq('{', repeat($._class_member), '}'),

    _class_member: ($) =>
      choice(
        $.constant_declaration,
        $.field_declaration,
        $.method_declaration,
        $.operator_declaration,
        $.constructor_declaration,
        $.destructor_declaration,
        $.friend_declaration,
        $.comptime_assert_statement,
      ),

    constant_declaration: ($) =>
      seq(
        repeat($.modifier),
        choice('const', 'comptime'),
        field('type', $._type),
        commaSep1($.constant_declarator),
        ';',
      ),

    field_declaration: ($) =>
      seq(
        optional($.attribute_list),
        repeat($.modifier),
        field('type', $._type),
        commaSep1($.variable_declarator),
        ';',
      ),

    method_declaration: ($) =>
      seq(
        repeat($.modifier),
        choice(seq('comptime', 'fn'), seq(optional('const'), 'fn')),
        field('return_type', $._function_return_type),
        field('name', $.method_name),
        field('parameters', $.parameter_list),
        optional($.when_clause),
        // A contract method has NO body — just `;`.
        field('body', choice($.block, ';')),
      ),

    // kama.y:980 — `copy`/`give` are hand-off markers only in EXPRESSION position, so they may also name a
    // member. This is what lets a `resource` opt into `Copyable` with a method literally named `copy`.
    // A named node, so a query colours it as a function rather than a keyword without pattern ordering.
    method_name: ($) => choice($.identifier, 'copy', 'give'),

    operator_declaration: ($) =>
      seq(
        repeat($.modifier),
        choice(
          // `ref T operator[](usize i)` — the place-returning index operator.
          seq(
            'ref',
            field('return_type', $._type),
            'operator',
            field('operator', $.index_operator),
            field('parameters', $.parameter_list),
          ),
          seq(
            field('return_type', $._type),
            'operator',
            field('operator', $.overloadable_operator),
            field('parameters', $.parameter_list),
          ),
        ),
        field('body', choice($.block, ';')),
      ),

    index_operator: ($) => seq('[', ']'),
    overloadable_operator: ($) => choice(...OVERLOADABLE_OPERATORS),

    constructor_declaration: ($) =>
      seq(
        repeat($.modifier),
        choice(
          // The classic form: `Name(params) : base(args) { }`
          seq(
            field('name', $.identifier),
            field('parameters', $.parameter_list),
            optional($.constructor_initializer),
            field('body', choice($.block, ';')),
          ),
          // The NAMED form: `ctor make(params) { }` / `ctor Result<This,E> make(params) { }`
          seq(
            'ctor',
            optional(field('return_type', $._type)),
            field('name', $.method_name),
            field('parameters', $.parameter_list),
            optional($.when_clause),
            field('body', choice($.block, ';')),
          ),
        ),
      ),

    constructor_initializer: ($) => seq(':', 'base', $.argument_list),

    destructor_declaration: ($) =>
      seq(
        repeat($.modifier),
        '~',
        field('name', $.identifier),
        '(',
        ')',
        field('body', $.block),
      ),

    // kama.y:309 — SQUARE brackets, and `[...]` means "all privates".
    friend_declaration: ($) =>
      seq(
        'friend',
        field('type', $.type_name),
        '[',
        choice(commaSep1($.identifier), '...'),
        ']',
        ';',
      ),

    // `virtual(maxDepth: 2)` / `abstract(maxDepth: 1)` — the extension budget, kama.y `modifier`. It
    // reuses `argument_list` there for the same reason it does here: the spelling is kama's ordinary
    // named-argument one, and the emitter (not the grammar) checks the name and the value.
    modifier: ($) =>
      choice(
        ...MODIFIERS,
        seq(choice('virtual', 'abstract'), $.argument_list),
      ),

    // ── Attributes ──────────────────────────────────────────────────────────────────────────────────
    // kama.y:729. Attribute arguments are the ONLY near-positional argument form in the language.
    attribute_list: ($) => repeat1($.attribute),

    attribute: ($) =>
      seq(
        '@',
        field('name', $.identifier),
        optional(seq('(', commaSep1($.attribute_argument), ')')),
      ),

    attribute_argument: ($) =>
      choice(
        seq(field('name', $.identifier), ':', $._expression),
        // `@compileFor(!RELEASE)` — negation is spelled in the attribute, not in the grammar at large.
        seq(optional('!'), $.identifier),
        $.string_literal,
      ),

    // ── Types ───────────────────────────────────────────────────────────────────────────────────────
    _type: ($) => choice($.primitive_type, $.type_name),

    primitive_type: ($) => choice(...PRIMITIVE_TYPES),

    type_name: ($) =>
      seq(
        repeat(seq($.identifier, '::')),
        field('name', $.identifier),
        optional($.type_arguments),
      ),

    // The USE-site argument list. This is where `>>` must split; see the header note.
    type_arguments: ($) => seq('<', commaSep1($._type_or_value_argument), '>'),

    _type_or_value_argument: ($) =>
      choice(
        $._type,
        // A NAMED override, `A: Arena`, skipping an earlier default.
        seq(field('name', $.identifier), ':', $._type),
        $._literal,
        // `InlineArray<T, (N+1)>` — parenthesised const arithmetic. The parens are what keep `>`/`>>`
        // unambiguous against the generic close.
        seq('(', $._expression, ')'),
      ),

    _qualified_name: ($) => seq($.identifier, repeat(seq('::', $.identifier))),

    // ── Statements ──────────────────────────────────────────────────────────────────────────────────
    block: ($) => seq('{', repeat($._statement), '}'),

    _statement: ($) =>
      choice(
        $.local_variable_declaration,
        $.local_constant_declaration,
        $.block,
        $.empty_statement,
        $.expression_statement,
        $.if_statement,
        $.while_statement,
        $.do_statement,
        $.for_statement,
        $.foreach_statement,
        $.break_statement,
        $.continue_statement,
        $.return_statement,
        $.arm_value_statement,
        $.unsafe_statement,
        $.spawn_statement,
        $.scope_statement,
        $.parallel_for_statement,
        $.asm_statement,
        $.comptime_assert_statement,
      ),

    // C-style: `int32 i = 0, j;`. There is no `let`/`var`.
    // `slot T x;` — a declared HOLE (no value yet, no destructor until assigned). A leading keyword,
    // like `const`/`comptime` below.
    local_variable_declaration: ($) =>
      seq(
        optional('slot'),
        field('type', $._type),
        commaSep1($.variable_declarator),
        ';',
      ),

    local_constant_declaration: ($) =>
      seq(
        choice('const', 'comptime'),
        field('type', $._type),
        commaSep1($.constant_declarator),
        ';',
      ),

    variable_declarator: ($) =>
      seq(
        field('name', $.identifier),
        optional(seq('=', field('value', $._variable_initializer))),
      ),

    // `Isolate h = spawn worker(p: give x);` — the handle form. `spawn` is an initializer only here.
    _variable_initializer: ($) => choice($._expression, $.spawn_expression),

    spawn_expression: ($) => seq('spawn', $.call_expression),

    constant_declarator: ($) =>
      seq(
        field('name', $.identifier),
        optional(seq('=', field('value', $._expression))),
      ),

    empty_statement: ($) => ';',

    // kama.y:499 — RESTRICTED, and the restriction is load-bearing: because `a < b;` is not a legal
    // statement, a leading `IDENT <` can only ever open a generic type, which is what lets `>>` split
    // inside type arguments without an external scanner.
    expression_statement: ($) => seq($._statement_expression, ';'),

    _statement_expression: ($) =>
      choice(
        $.call_expression,
        $.object_creation_expression,
        $.assignment_expression,
        $.update_expression,
        $.match_expression,
      ),

    // MANDATORY BRACES — every branch and loop body is a `block`, never a bare statement. kama.y raises
    // "the body of `if` must be braced" as a PARSE error, so tree-sitter must reject the same inputs:
    // test/parse-errors.txt is DERIVED from the compiler's parse-vs-semantic split, and this rule lands
    // on the parse side. The editor flagging a bare body live is the point, not a side effect.
    //
    // `else` is the one exemption, and it is not a bare body: the alternative may be a block OR another
    // `if`, which is an `else if` chain link. prec.right still binds an `else` to the nearest `if`.
    if_statement: ($) =>
      prec.right(
        seq(
          'if',
          '(',
          field('condition', $._expression),
          ')',
          field('consequence', $.block),
          optional(
            seq('else', field('alternative', choice($.block, $.if_statement))),
          ),
        ),
      ),

    while_statement: ($) =>
      seq(
        'while',
        '(',
        field('condition', $._expression),
        ')',
        field('body', $.block),
      ),

    do_statement: ($) =>
      seq(
        'do',
        field('body', $.block),
        'while',
        '(',
        field('condition', $._expression),
        ')',
        ';',
      ),

    for_statement: ($) =>
      seq(
        'for',
        '(',
        optional(
          choice($._for_initializer_declaration, $._statement_expression_list),
        ),
        ';',
        field('condition', optional($._expression)),
        ';',
        optional($._statement_expression_list),
        ')',
        field('body', $.block),
      ),

    _for_initializer_declaration: ($) =>
      seq(field('type', $._type), commaSep1($.variable_declarator)),

    _statement_expression_list: ($) => commaSep1($._statement_expression),

    foreach_statement: ($) =>
      seq(
        'foreach',
        '(',
        optional('ref'),
        field('type', $._type),
        field('name', $.identifier),
        'in',
        field('collection', $._expression),
        ')',
        field('body', $.block),
      ),

    // kama.y:490 — `ref` is MANDATORY (disjoint mutable access is the whole point) and the body must be a
    // block, because those braces ARE the join barrier.
    parallel_for_statement: ($) =>
      seq(
        'parallel_for',
        '(',
        'ref',
        field('type', $._type),
        field('name', $.identifier),
        'in',
        field('collection', $._expression),
        ')',
        field('body', $.block),
      ),

    break_statement: ($) => seq('break', ';'),
    continue_statement: ($) => seq('continue', ';'),
    return_statement: ($) => seq('return', optional($._expression), ';'),

    // `:= expr;` — the value a match arm's BLOCK produces. A statement, not a jump.
    arm_value_statement: ($) => seq(':=', $._expression, ';'),

    unsafe_statement: ($) => seq('unsafe', field('body', $.block)),
    scope_statement: ($) => seq('scope', field('body', $.block)),
    spawn_statement: ($) => seq('spawn', $.call_expression, ';'),

    // Exactly ONE string-literal operand, and it is embedded assembly — never interpolated.
    asm_statement: ($) => seq('asm', '(', field('code', $.string_literal), ')', ';'),

    // kama.y `comptime_assert_statement`. `comptime assert(cond: …, msg: "…");` — a compile-time
    // assertion (M7), legal at module, type-member and statement scope. `assert` is an ordinary
    // IDENTIFIER, not a keyword (the runtime `assert` builtin is spelled the same way), so the callee is
    // matched as one rather than as a literal token.
    comptime_assert_statement: ($) =>
      seq(
        'comptime',
        field('callee', $.identifier),
        $.argument_list,
        ';',
      ),

    // ── match ───────────────────────────────────────────────────────────────────────────────────────
    // kama.y:561 — both a statement (value discarded, trailing `;`) and an expression. Arms use a COLON;
    // an expression arm needs a trailing `;`, a block arm yields via `:= expr;`.
    match_expression: ($) =>
      seq(
        'match',
        '(',
        field('subject', $._expression),
        ')',
        '{',
        repeat($.match_arm),
        '}',
      ),

    match_arm: ($) =>
      seq(
        'case',
        field('pattern', $.match_pattern),
        ':',
        field('body', choice(seq($._expression, ';'), $.block)),
      ),

    // No guards, no literal patterns, no alternatives, no nesting. `_` is just an identifier.
    // A payload pattern NAMES its fields — `case Rect(w: width, h: height)` — exactly as a call names
    // its parameters. There is no positional form.
    match_pattern: ($) =>
      seq(
        field('name', $.identifier),
        optional(seq('(', commaSep1($.match_binding), ')')),
      ),

    match_binding: ($) =>
      seq(field('field', $.identifier), ':', field('name', $.identifier)),

    // ── Expressions ─────────────────────────────────────────────────────────────────────────────────
    _expression: ($) =>
      choice(
        $._literal,
        $.identifier,
        $.scoped_identifier,
        $.this_expression,
        $.base_expression,
        $.parenthesized_expression,
        $.field_expression,
        $.call_expression,
        $.subscript_expression,
        $.object_creation_expression,
        $.array_literal,
        $.downcast_expression,
        $.match_expression,
        $.unary_expression,
        $.update_expression,
        $.binary_expression,
        $.ternary_expression,
        $.assignment_expression,
        $.handoff_expression,
        $.cast_expression,
        $.bitcast_expression,
        $.sizeof_expression,
      ),

    this_expression: ($) => 'this',

    base_expression: ($) =>
      seq(
        'base',
        choice(
          seq('.', field('field', $.identifier)),
          seq('[', commaSep1($._expression), ']'),
        ),
      ),

    parenthesized_expression: ($) => seq('(', $._expression, ')'),

    scoped_identifier: ($) =>
      prec.left(
        seq(
          field('scope', choice($.identifier, $.scoped_identifier)),
          '::',
          field('name', $.identifier),
        ),
      ),

    field_expression: ($) =>
      prec.left(
        PREC.member,
        seq(field('object', $._expression), '.', field('field', $.identifier)),
      ),

    subscript_expression: ($) =>
      prec.left(
        PREC.member,
        seq(field('object', $._expression), '[', commaSep1($._expression), ']'),
      ),

    // Callees are restricted the way kama.y restricts them — a parenthesised expression is not callable.
    _callable: ($) =>
      choice(
        $.identifier,
        $.scoped_identifier,
        $.field_expression,
        $.subscript_expression,
        // `base.kind()` — the non-virtual base call. kama.y puts `base_access` in
        // `primary_expression_no_parenthesis`, which `invocation_expression` calls; leaving it out here
        // is what the whole-corpus oracle caught on its first run (tests/vtable_depth3.kama).
        $.base_expression,
        $.turbofish_name,
        $.turbofish_type_member,
        $.turbofish_member,
        $.primitive_type, // `string.fromInt(...)` — kama.y `class_type DOT IDENTIFIER`
      ),

    call_expression: ($) =>
      prec.left(
        PREC.call,
        seq(field('function', $._callable), field('arguments', $.argument_list)),
      ),

    // `f::<int32>()` and `Map::<K,V>.empty()` share this prefix (kama.y `generic_turbofish_name`). It stays
    // PURE — the trailing `.member` belongs to the call/creation site, not here, or the two spellings both
    // try to consume the dot.
    turbofish_name: ($) =>
      seq(field('name', $.identifier), '::', $.type_arguments),

    // `Map::<K,V>.empty()` — the on-type turbofish, the canonical generic-constructor spelling.
    // `::` is the STATIC form of the same thing (`Box::<int32>::tag()`, kama.y:1200) — leaving it out is
    // what the whole-corpus oracle caught on tests/generic_static.kama and tests/idioms_kama_way.kama.
    turbofish_type_member: ($) =>
      seq($.turbofish_name, choice('.', '::'), field('member', $.identifier)),

    // `r.deserialize::<T>()` — a receiver turbofish. The `::` is what disambiguates from `<` as less-than.
    turbofish_member: ($) =>
      prec.left(
        PREC.member,
        seq(
          field('object', $._expression),
          '.',
          field('name', $.identifier),
          '::',
          $.type_arguments,
        ),
      ),

    argument_list: ($) => seq('(', commaSep($.argument), ')'),

    // THE defining shape of a kama call: every argument is named. There is deliberately NO positional
    // alternative here — `f(1)` must be an ERROR.
    argument: ($) =>
      seq(
        field('name', $.identifier),
        ':',
        optional(choice('ref', 'out')),
        field('value', $._expression),
      ),

    object_creation_expression: ($) =>
      prec.right(
        seq(
          optional('try'),
          'new',
          // `new(allocator: a) T(...)` — placement.
          optional(field('placement', $.argument_list)),
          field('type', choice($._type, $.turbofish_name)),
          optional(seq('.', field('constructor', $.identifier))),
          field('arguments', $.argument_list),
        ),
      ),

    // `[a, b, c]` (elements) or `[v; N]` (fill).
    array_literal: ($) =>
      seq(
        '[',
        choice(
          commaSep1($._expression),
          seq($._expression, ';', field('count', $._expression)),
        ),
        ']',
      ),

    // `expr.as<T>()` — the runtime downcast, yielding Optional<T>.
    downcast_expression: ($) =>
      prec.left(
        PREC.member,
        seq(
          field('object', $._expression),
          '.',
          'as',
          '<',
          field('type', $._type),
          '>',
          '(',
          ')',
        ),
      ),

    cast_expression: ($) =>
      seq('cast', '<', field('type', $._type), '>', '(', field('value', $._expression), ')'),

    bitcast_expression: ($) =>
      seq('bitcast', '<', field('type', $._type), '>', '(', field('value', $._expression), ')'),

    // These take a TYPE, not an expression.
    sizeof_expression: ($) =>
      seq(choice('sizeof', 'alignof'), '(', field('type', $._type), ')'),

    unary_expression: ($) =>
      prec.right(
        PREC.unary,
        seq(
          field('operator', choice('!', '~', '-', '+')),
          field('operand', $._expression),
        ),
      ),

    update_expression: ($) =>
      choice(
        prec.right(
          PREC.unary,
          seq(
            field('operator', choice('++', '--')),
            field('operand', $._expression),
          ),
        ),
        prec.left(
          PREC.postfix,
          seq(
            field('operand', $._expression),
            field('operator', choice('++', '--')),
          ),
        ),
      ),

    // The flattened cascade. Each line is one kama.y nonterminal; see PREC above.
    binary_expression: ($) => {
      const table = [
        [PREC.multiplicative, choice('*', '/', '%')],
        [PREC.additive, choice('+', '-')],
        [PREC.shift, choice('<<', '>>')],
        [PREC.relational, choice('<', '>', '<=', '>=')],
        [PREC.equality, choice('==', '!=')],
        [PREC.bitand, '&'],
        [PREC.bitxor, '^'],
        [PREC.bitor, '|'],
        [PREC.and, '&&'],
        [PREC.or, '||'],
      ];
      return choice(
        ...table.map(([precedence, operator]) =>
          prec.left(
            precedence,
            seq(
              field('left', $._expression),
              field('operator', operator),
              field('right', $._expression),
            ),
          ),
        ),
      );
    },

    ternary_expression: ($) =>
      prec.right(
        PREC.ternary,
        seq(
          field('condition', $._expression),
          '?',
          field('consequence', $._expression),
          ':',
          field('alternative', $._expression),
        ),
      ),

    // kama.y:620 is `assignment : unary_expression assignment_operator expression` — the LHS is a UNARY
    // expression, NOT a full one. That is not a detail: if the LHS were `_expression`, then a statement
    // could begin with a binary expression, `a < b` would be reachable at statement position, and the
    // whole `>>`-splitting argument in the header note would collapse into an unresolvable conflict
    // between `type_name` and `_expression`. Keep this tight.
    assignment_expression: ($) =>
      prec.right(
        PREC.assignment,
        seq(
          field('left', $._expression),
          field(
            'operator',
            choice('=', '+=', '-=', '*=', '/=', '%=', '^=', '&=', '|=', '>>=', '<<='),
          ),
          field('right', $._expression),
        ),
      ),

    // `give x` / `copy x` — move and duplicate markers, at the loosest precedence.
    handoff_expression: ($) =>
      prec.right(
        PREC.handoff,
        seq(
          field('operator', choice('give', 'copy')),
          field('value', $._expression),
        ),
      ),

    // ── Literals ────────────────────────────────────────────────────────────────────────────────────
    _literal: ($) =>
      choice(
        $.boolean_literal,
        $.null_literal,
        $.integer_literal,
        $.float_literal,
        $.char_literal,
        $.string_literal,
        $.verbatim_string_literal,
        $.tagged_string_literal,
      ),

    boolean_literal: ($) => choice('true', 'false'),
    null_literal: ($) => 'null',

    // Transcribed literally from kama.l:68-80. The unsigned suffix is `ui`, NOT `u` — there is no `42u32`.
    // A based literal REQUIRES its `_<base>` tail, so bare `0b1010` is `0` followed by the identifier
    // `b1010` and the enclosing statement fails to parse. There are NO digit separators.
    //
    // Float is listed FIRST in `_literal` order-independently, but the token conflict matters: `1.5` must
    // not lex as `1` `.` `5`. tree-sitter resolves by longest match, and float_literal is longer.
    integer_literal: ($) =>
      token(
        seq(
          choice(
            /0[xX][0-9A-Fa-f]+/,
            /0[oO][0-7]+/,
            /0[bB][0-9A-Za-z]+_([1-2][0-9]|[3][0-2]|[2-9])/,
            /[0-9]+/,
          ),
          optional(/u?i(8|16|32|64)/),
        ),
      ),

    // kama.l:91-94. A dot needs digits on BOTH sides (`1.` and `.5` are rejected); a dotless form needs an
    // exponent (`1e10` is accepted). Suffixes are only `f32`/`f64`.
    float_literal: ($) =>
      token(
        seq(
          choice(/[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?/, /[0-9]+[eE][+-]?[0-9]+/),
          optional(/f(32|64)/),
        ),
      ),

    // kama.l:96-107. tree-sitter regexes match CODE POINTS, not bytes, so `[^\\']` covers both the plain
    // single-byte form and the multibyte one (`'e'`, emoji) — and rejects `'abc'` and `''` for free.
    char_literal: ($) =>
      token(
        seq(
          "'",
          choice(/[^\\']/, /\\['"\\0abfnrtv$]/, /\\u\{[0-9A-Fa-f]{1,6}\}/),
          "'",
        ),
      ),

    // A regular string. Every piece is `token.immediate` so tree-sitter does not skip `extras` (i.e. eat
    // the whitespace) inside the literal.
    string_literal: ($) =>
      seq(
        '"',
        repeat(
          choice(
            $.string_content,
            $.escape_sequence,
            $.interpolation,
            $._lone_dollar,
          ),
        ),
        token.immediate('"'),
      ),

    // kama.l `single_string_char` is `[^\\"]`, which INCLUDES newline — a plain "..." may legally span
    // lines — and includes `$`. `$` is excluded here only so that `${` can win by longest match below,
    // exactly as it does in flex; a lone `$` comes back via `_lone_dollar`.
    string_content: ($) => token.immediate(prec(1, /[^"\\$]+/)),

    // The escape set is EXACTLY kama.l's. Anything else (`\e`, `\x41`, `\q`) is a lexical error in the
    // compiler, so it must fail to parse here too — that is what makes tests/syntax/bad.kama meaningful.
    escape_sequence: ($) =>
      token.immediate(choice(/\\['"\\0abfnrtv$]/, /\\u\{[0-9A-Fa-f]{1,6}\}/)),

    _lone_dollar: ($) => token.immediate('$'),

    // tree-sitter regexes are DFAs with no lookahead, so "a `$` not followed by `{`" is unwritable. It does
    // not need to be: both `$` and `${` are valid here and maximal munch picks `${`.
    interpolation: ($) =>
      seq(
        token.immediate('${'),
        $.interpolation_expression,
        optional(seq(':', $.format_spec)),
        '}',
      ),

    // RESTRICTED to an identifier with `.field` / `[index]` accessors. No operators, no calls — kama.l
    // errors on anything else, and tests/xfail/interp_hole_operator.kama is one of the files the
    // whole-corpus oracle requires to fail. Whitespace inside the hole IS ignored (kama.l:247).
    interpolation_expression: ($) =>
      seq(
        $.identifier,
        repeat(
          choice(
            seq('.', $.identifier),
            seq('[', choice($.identifier, $.integer_literal), ']'),
          ),
        ),
      ),

    format_spec: ($) => token.immediate(/[-+.0-9A-Za-z]+/),

    // `@"..."` — spans newlines, `""` is the ONLY escape, no interpolation and no backslash escapes.
    // Both `""` and `"` are valid at the closing position; longest match takes `""`, exactly as flex does.
    // This is the tie the TextMate grammar got wrong (its defect #2).
    verbatim_string_literal: ($) =>
      seq(
        '@"',
        repeat(choice($.verbatim_content, $.quote_escape)),
        token.immediate('"'),
      ),

    verbatim_content: ($) => token.immediate(prec(1, /[^"]+/)),
    quote_escape: ($) => token.immediate('""'),

    // `html"..."` / `sql"..."` — an identifier IMMEDIATELY adjacent to the opening quote. `foo "x"` with a
    // space is NOT a tag, which is why the quote is `token.immediate`.
    tagged_string_literal: ($) =>
      seq(
        field('tag', $.string_tag),
        field('body', choice($._immediate_string, $._immediate_verbatim)),
      ),

    string_tag: ($) => $.identifier,

    _immediate_string: ($) =>
      seq(
        token.immediate('"'),
        repeat(
          choice(
            $.string_content,
            $.escape_sequence,
            $.interpolation,
            $._lone_dollar,
          ),
        ),
        token.immediate('"'),
      ),

    _immediate_verbatim: ($) =>
      seq(
        token.immediate('@"'),
        repeat(choice($.verbatim_content, $.quote_escape)),
        token.immediate('"'),
      ),

    // ── Lexical ─────────────────────────────────────────────────────────────────────────────────────
    // kama.l:119-121 — ASCII only, no `$`, no Unicode identifiers. A bare `_` is a valid identifier, which
    // is how the match wildcard `case _:` parses.
    identifier: ($) => /[A-Za-z_][A-Za-z0-9_]*/,

    line_comment: ($) => token(seq('//', /[^\n]*/)),

    // Block comments do NOT nest: kama.l's IN_COMMENT is an exclusive state whose only rules are `.`,
    // newline and `*/`, so an inner `/*` is just two ordinary characters.
    block_comment: ($) => token(seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/')),

    preproc_line: ($) => token(seq('#', /[^\n]*/)),
  },
});
