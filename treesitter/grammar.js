/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// The alcy grammar for tree-sitter.
//
// The normative grammar is docs/spec/grammar.ebnf, and this file follows it
// production for production. What EBNF cannot say lives in
// docs/spec/grammar.md, and every such rule that the tree has to agree with
// is called out in a comment below.

// Binding power of each expression level, lowest first. The chain mirrors
// grammar.ebnf: a range sits below the logical operators, a cast above `**`,
// and a postfix operator above everything unary.
const PREC = {
  OR: 1,
  AND: 2,
  COMPARISON: 3,
  BIT_OR: 4,
  BIT_XOR: 5,
  BIT_AND: 6,
  SHIFT: 7,
  ADD: 8,
  MUL: 9,
  POW: 10,
  CAST: 11,
  UNARY: 12,
  POSTFIX: 14,
};

const DIGIT = /[0-9]/;
const HEX_DIGIT = /[0-9a-fA-F]/;
const DECIMAL_DIGIT = /[0-9_]/;
// A suffix starts at the first letter or underscore run, and an unknown one
// passes through to be rejected later, as it does in the lexer.
const SUFFIX = /[A-Za-z_][A-Za-z0-9_]*/;

module.exports = grammar({
  name: 'alcy',

  extras: $ => [
    /\s/,
    $.line_comment,
    $.block_comment,
    $.doc_comment,
  ],

  // `_block_lbrace` is the opening brace of a block whose header it belongs
  // to, which the scanner produces only when the brace stays on the header
  // line (grammar.md, "Block openers stay on their header line"). The
  // scanner is asked for it before any other token, so in a header position
  // it wins over the plain `{` a struct literal would use, which is what
  // keeps `if c { }` a block rather than a struct literal on `c`. A block
  // used as an expression in its own right has no header, so it takes a
  // plain `{` and `{ ... }` on a line of its own is a block-valued
  // statement.
  //
  // `_newline` is the `;` the lexer inserts at the end of a line
  // (grammar.md, "Newlines are significant"). The scanner produces it only
  // where a statement can end, so the "may this line end a statement" half
  // of the rule is carried by where the token appears in this grammar and
  // the "does the next token continue it" half by the scanner.
  //
  // `block_comment` is external because block comments nest, which no
  // regular expression can express.
  //
  // `_error_sentinel` is never produced; it exists so the scanner is never
  // running with no legal token to return.
  externals: $ => [
    $._block_lbrace,
    $._newline,
    $.block_comment,
    $._error_sentinel,
  ],

  word: $ => $.identifier,

  // A statement is a declaration, an assignment, or an expression, and which
  // one it is turns on a `:=` or an `=` further along the line than the
  // first token. The compiler decides the same way, by looking ahead at
  // bracket depth zero; here both readings are explored and the one that
  // finishes is the one that survives.
  conflicts: $ => [
    [$.tuple_variant_pattern, $.path_expression],
    [$.unit_variant_pattern, $.path_expression],
    [$.literal_pattern, $._primary_expression],
    [$.negative_literal_pattern, $._literal],
    // `a < b` is a comparison and `a<b>` is a path with type arguments, and
    // the `<` alone cannot say which. Whichever reading reaches its `>` (or
    // reaches the end of the expression without one) is the one kept.
    [$.path],
    // Likewise `..` opens a range around an operand rather than continuing
    // an expression that already ended, and only the range reaches the
    // token after it.
    [$._expression, $.range_expression],
    // An `if` reads a newline before its `else` as part of itself rather
    // than as the end of the statement it is in.
    [$.if_expression],
    // `(` opens a pattern and a closure parameter alike, and `mut` does
    // not settle it: the `->` after the `)` is what tells them apart.
    [$._primary_pattern, $.closure_parameter],
    [$.mut_identifier_pattern, $.closure_parameter],
    [$.closure_parameter, $._path_segment],
    // `[a, b]` is an array and the capture list of `[a, b] (p) -> body`;
    // the `(` after the `]` is what tells them apart, and `self` in
    // either position reads as a path segment or a capture until then.
    [$.capture, $._path_segment],
    // `(A, B)` is a tuple type and the parameter list of `(A, B) -> R`;
    // the `->` after the `)` is what tells them apart.
    [$.tuple_type, $.function_parameters],
  ],

  supertypes: $ => [
    $._expression,
    $._type,
    $._pattern,
    $._literal,
  ],

  rules: {
    // A stray `;` between items is a no-op (grammar.md).
    source_file: $ => itemList($, $._item),

    // Items ----------------------------------------------------------------

    _item: $ => choice(
      $.function_item,
      $.intrinsic_fn_item,
      $.extern_item,
      $.struct_item,
      $.enum_item,
      $.impl_item,
      $.spec_item,
      $.static_item,
      $.const_item,
      $.use_item,
    ),

    visibility: _ => 'pub',

    // `unsafe` marks an operation, so it is read only where a function
    // declares one (ffi.md, "The gate"); the body of an `unsafe fn` is
    // not an unsafe context implicitly.
    function_item: $ => seq(
      optional(field('visibility', $.visibility)),
      optional('unsafe'),
      'fn',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      field('parameters', $.parameters),
      optional(field('return_type', $.return_type)),
      field('body', $.block),
    ),

    // A signature without a body, terminated by `;` (items.md, "Intrinsic
    // declarations"). The precondition-carrying intrinsics say `unsafe`.
    intrinsic_fn_item: $ => seq(
      optional(field('visibility', $.visibility)),
      optional('unsafe'),
      'intrinsic',
      'fn',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      field('parameters', $.parameters),
      optional(field('return_type', $.return_type)),
      ';',
    ),

    // `extern "C" { ... }` declares bodyless functions whose names are
    // the symbols the linker resolves (ffi.md, "External functions").
    // The convention is part of the declaration and only "C" is
    // defined; a grammar without a checker reads any string the same way.
    extern_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'extern',
      field('convention', $.string),
      $._block_lbrace,
      itemList($, $.extern_fn_item),
      '}',
    ),

    // A declaration, not a definition: no `pub` or `unsafe` marker of
    // its own, and the signature ends at the `;`.
    extern_fn_item: $ => seq(
      'fn',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      field('parameters', $.parameters),
      optional(field('return_type', $.return_type)),
      ';',
    ),

    struct_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'struct',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      $._block_lbrace,
      // A struct with no fields is valid, like an empty block.
      optional(sepByTrailing($.field_declaration, ',')),
      '}',
    ),

    field_declaration: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
    ),

    enum_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'enum',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      $._block_lbrace,
      // An enum with no variants is valid, like an empty block.
      optional(sepByTrailing($.enum_variant, ',')),
      '}',
    ),

    // Unit and tuple variants only; a struct variant is deferred.
    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(seq('(', optional(sepByTrailing($._type, ',')), ')')),
    ),

    // `impl S for T` names the spec first and the target second; without
    // `for` the block is inherent and `type` is the only type.
    impl_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'impl',
      optional(field('type_parameters', $.type_parameters)),
      optional(seq(field('spec', $._type), 'for')),
      field('type', $._type),
      $._block_lbrace,
      itemList($, $.function_item),
      '}',
    ),

    // A path after `:` names a super-spec: every implementation of this
    // spec then also requires an implementation of the super for the
    // same target (grammar.ebnf, `spec_item`).
    spec_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'spec',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      optional(seq(':', field('super', $.path_type))),
      $._block_lbrace,
      itemList($, $.spec_method),
      '}',
    ),

    // Signatures only: a body here is an error, and every implementation
    // supplies each declared method. An `unsafe` marker parses and the
    // checker refuses it for now (ffi.md).
    spec_method: $ => seq(
      optional(field('visibility', $.visibility)),
      optional('unsafe'),
      'fn',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      field('parameters', $.parameters),
      optional(field('return_type', $.return_type)),
      ';',
    ),

    static_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'static',
      field('name', $.identifier),
      ':',
      field('type', $._type),
      '=',
      field('value', $._expression),
    ),

    const_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'const',
      field('name', $.identifier),
      ':',
      field('type', $._type),
      '=',
      field('value', $._literal),
    ),

    use_item: $ => seq(
      optional(field('visibility', $.visibility)),
      'use',
      field('path', $.path),
      optional(seq('as', field('alias', $.identifier))),
      ';',
    ),

    return_type: $ => seq('->', field('type', $._type)),

    type_parameters: $ => seq(
      '<',
      sepByTrailing(field('name', $.identifier), ','),
      '>',
    ),

    parameters: $ => seq(
      '(',
      optional(sepByTrailing($.parameter, ',')),
      ')',
    ),

    // `comp` marks a compile-time parameter; plain functions use the same
    // shape without it (comp.md).
    parameter: $ => seq(
      optional(field('comp', 'comp')),
      field('pattern', $._pattern),
      ':',
      field('type', $._type),
    ),

    // Types ----------------------------------------------------------------

    _type: $ => choice(
      $.primitive_type,
      $.unit_type,
      $.never_type,
      $.str_type,
      $.tuple_type,
      $.paren_type,
      $.function_type,
      $.array_type,
      $.slice_type,
      $.reference_type,
      $.raw_ptr_type,
      $.path_type,
    ),

    primitive_type: _ => token(choice(
      'i8', 'i16', 'i32', 'i64', 'i128', 'isize',
      'u8', 'u16', 'u32', 'u64', 'u128', 'usize',
      'f32', 'f64', 'bool',
    )),

    unit_type: _ => seq('(', ')'),

    never_type: _ => '!',

    str_type: _ => 'str',

    tuple_type: $ => seq(
      '(',
      $._type,
      ',',
      // The comma is what makes the parens a tuple, so `(A,)` is a
      // one-element tuple while `(A)` is `A`; the types after the first
      // comma are optional. Right-associative: a further comma binds to
      // the type that follows it, so this prefix is shared with a
      // function's parameters and a variant's fields until the `)` (or
      // the `->`) says which one it is.
      optional(seq($._type, repeat(prec.right(seq(',', $._type))),
                  optional(','))),
      ')',
    ),

    // A single type in parens is that type; the arrow after the `)` is
    // what makes it a function type instead (grammar.ebnf, "Types").
    paren_type: $ => seq('(', $._type, ')'),

    // `(A, B) -> R`; `()` and `(A)` are the no- and one-parameter
    // spellings (grammar.ebnf, "Types").
    function_type: $ => seq(
      field('parameters', $.function_parameters),
      '->',
      field('return', $._type),
    ),

    function_parameters: $ => seq(
      '(',
      optional(sepByTrailing($._type, ',')),
      ')',
    ),

    array_type: $ => seq(
      '[',
      field('element', $._type),
      ';',
      field('length', $.integer),
      ']',
    ),

    slice_type: $ => seq(
      '[',
      field('element', $._type),
      ']',
    ),

    reference_type: $ => seq(
      '&',
      optional('mut'),
      field('type', $._type),
    ),

    // A raw pointer is thin and Copy, outside the region system
    // (ffi.md). The outer star is shared and a `mut` after the pair
    // binds to the inner one, which is what `*(*mut T)` says spelled
    // out; a type position has no power, so the `**` pair is two stars.
    raw_ptr_type: $ => seq(
      '*',
      optional('mut'),
      field('type', $._type),
    ),

    path_type: $ => field('path', $.path),

    // A closing `>>` splits into two `>` (grammar.md, "Types"). The lexer
    // gets that for free: a shift operator is not a valid token inside a
    // type argument list, so only `>` is ever considered there.
    type_arguments: $ => seq(
      '<',
      sepByTrailing($._type, ','),
      '>',
    ),

    // Patterns -------------------------------------------------------------

    // `parse_pattern` is a run of primary patterns separated by `|`, and
    // everything else binds tighter than that run. So `&` takes a primary
    // pattern, while a field or a tuple-variant element takes a whole one.
    _pattern: $ => choice(
      $._primary_pattern,
      $.or_pattern,
    ),

    _primary_pattern: $ => choice(
      $.wildcard_pattern,
      $.mut_identifier_pattern,
      $.identifier_pattern,
      $.literal_pattern,
      $.negative_literal_pattern,
      $.tuple_pattern,
      $.tuple_variant_pattern,
      $.struct_pattern,
      $.unit_variant_pattern,
      $.reference_pattern,
    ),

    wildcard_pattern: _ => '_',

    // `mut self` receivers spell the name with the module keyword, so a
    // receiver is accepted here like an identifier. `self` binds tighter
    // than a path segment would, because a receiver never carries one: a
    // lone `self` is the receiver, and `self::member` is still a path.
    mut_identifier_pattern: $ => seq(
      'mut',
      field('name', choice($.identifier, alias('self', $.self))),
    ),

    identifier_pattern: $ => prec(1, field('name', choice($.identifier, alias('self', $.self)))),

    literal_pattern: $ => $._literal,

    // A `-` binds only to an integer or float literal, so `x - 1` never
    // parses as a pattern (grammar.md, "Patterns").
    negative_literal_pattern: $ => seq(
      '-',
      choice($.integer, $.float),
    ),

    tuple_pattern: $ => seq(
      '(',
      choice(
        $._pattern,
        // `(c,)` destructures into one element, and `(c)` is the same
        // pattern without the separator.
        seq(
          $._pattern,
          ',',
          optional(sepByTrailing($._pattern, ',')),
          optional(','),
        ),
      ),
      ')',
    ),

    // A bare path names a unit variant. The compiler decides between this
    // and `identifier_pattern` on the leading character of a lone segment;
    // here a single identifier is always a binding, which is the common
    // case and the one a query cares about.
    unit_variant_pattern: $ => field('path', $.path),

    tuple_variant_pattern: $ => seq(
      field('path', $.path),
      '(',
      optional(sepByTrailing($._pattern, ',')),
      ')',
    ),

    struct_pattern: $ => seq(
      field('path', $.path),
      '{',
      sepByTrailing($.field_pattern, ','),
      '}',
    ),

    // `Foo { x }` binds the field to a variable of the same name.
    field_pattern: $ => choice(
      seq(field('name', $.identifier), ':', field('pattern', $._pattern)),
      field('name', $.identifier),
    ),

    reference_pattern: $ => seq(
      '&',
      optional('mut'),
      field('pattern', $._primary_pattern),
    ),

    or_pattern: $ => seq(
      field('alternative', $._primary_pattern),
      repeat1(seq('|', field('alternative', $._primary_pattern))),
    ),

    // Expressions ----------------------------------------------------------

    _expression: $ => choice(
      $.range_expression,
      $._operand,
    ),

    // A range that names an end says how it is bound, so only `..=` and
    // `..<` may be followed by one, and a bare `..` names an unbounded end
    // (grammar.md, "Lexical notes"). Splitting the rule in two is what
    // keeps `1..2` out while `..<2` and `1..` still read as ranges.
    //
    // A range sits below every binary operator and is never one of their
    // operands, which is why its endpoints are `_operand` rather than an
    // `_expression`: `a || b .. c` is `(a || b) .. c`.
    range_expression: $ => choice(
      seq(
        optional(field('start', $._operand)),
        field('operator', '..'),
      ),
      seq(
        optional(field('start', $._operand)),
        field('operator', choice('..=', '..<')),
        field('end', $._operand),
      ),
    ),

    // Everything a binary operator takes as an operand, and therefore
    // everything except a range. One rule for the whole spine, with the
    // binding power carried per operator, is what keeps `a + b * c` from
    // parsing both ways.
    _operand: $ => choice(
      $.binary_expression,
      $.comparison_expression,
      $.cast_expression,
      $._unary_expression,
    ),

    // A cast takes a unary expression and so takes everything a unary
    // expression bottoms out at, which is the postfix chain: `f() as T`
    // casts the call, not nothing.
    _unary_expression: $ => choice(
      $.unary_expression,
      $.borrow_expression,
      $.deref_expression,
      $._postfix_expression,
    ),

    _postfix_expression: $ => choice(
      $._primary_expression,
      $.call_expression,
      $.method_call_expression,
      $.field_expression,
      $.index_expression,
      $.try_expression,
    ),

    binary_expression: $ => choice(
      ...[
        [PREC.OR, '||'],
        [PREC.AND, '&&'],
        [PREC.BIT_OR, '|'],
        [PREC.BIT_XOR, '^'],
        [PREC.BIT_AND, '&'],
        [PREC.SHIFT, choice('<<', '>>')],
        [PREC.ADD, choice('+', '-')],
        [PREC.MUL, choice('*', '/', '%')],
      ].map(([precedence, operator]) => prec.left(
        precedence,
        seq(
          field('left', $._operand),
          field('operator', operator),
          field('right', $._operand),
        ),
      )),
      // `**` is right-associative: `a ** b ** c` is `a ** (b ** c)`.
      prec.right(PREC.POW, seq(
        field('left', $._operand),
        field('operator', alias($._pow_operator, '**')),
        field('right', $._operand),
      )),
    ),

    // The power operator is two stars with the second one immediate
    // rather than a `**` token. A state that expects an operand has no
    // power, so `**p` and `**T` read as a pair of stars there, while
    // `a * *b` stays a multiplication of a dereference and `a **b` is
    // the power (grammar.md, "Expressions"). The alias keeps `**` a
    // single operator node, the way every other binary operator is.
    _pow_operator: $ => seq('*', token.immediate('*')),

    comparison_expression: $ => prec.left(PREC.COMPARISON, seq(
      field('left', $._operand),
      field('operator', choice('==', '!=', '>', '<', '>=', '<=')),
      field('right', $._operand),
    )),

    cast_expression: $ => prec.left(PREC.CAST, seq(
      field('value', $._unary_expression),
      'as',
      field('type', $._type),
    )),

    unary_expression: $ => prec.left(PREC.UNARY, seq(
      field('operator', choice('-', '!', '~')),
      field('argument', $._unary_expression),
    )),

    borrow_expression: $ => prec.left(PREC.UNARY, seq(
      '&',
      optional('mut'),
      field('value', $._unary_expression),
    )),

    deref_expression: $ => prec.left(PREC.UNARY, seq(
      '*',
      field('value', $._unary_expression),
    )),

    call_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('function', postfixOperand($)),
      field('arguments', $.arguments),
    )),

    method_call_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', postfixOperand($)),
      '.',
      field('name', $.identifier),
      field('arguments', $.arguments),
    )),

    // A `.` names a field by identifier or a tuple element by number.
    field_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', postfixOperand($)),
      '.',
      field('field', choice($.identifier, $.integer)),
    )),

    index_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', postfixOperand($)),
      '[',
      field('index', $._expression),
      ']',
    )),

    try_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('value', postfixOperand($)),
      '?',
    )),

    arguments: $ => seq(
      '(',
      optional(sepByTrailing($._expression, ',')),
      ')',
    ),

    _primary_expression: $ => choice(
      $._literal,
      $.path_expression,
      $.struct_expression,
      $.unit_expression,
      $.tuple_expression,
      $.array_expression,
      $.repeat_expression,
      $.parenthesized_expression,
      $.closure_expression,
      $.block_expression,
      $.comp_block,
      $.unsafe_block,
      $.if_expression,
      $.match_expression,
      $.loop_expression,
      $.while_expression,
      $.for_expression,
      $.return_expression,
      $.break_expression,
      $.continue_expression,
    ),

    path_expression: $ => field('path', $.path),

    // A brace after a path continues the expression rather than ending the
    // statement it sits in, so this binds like the postfix operators it sits
    // among. Without that, `p := Point { x: 1 }` reads as a declaration of
    // `p` followed by a block-valued statement.
    struct_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('path', $.path),
      '{',
      sepByTrailing($.field_initializer, ','),
      // `..base` move-update.
      optional(seq('..', field('base', $._expression))),
      '}',
    )),

    field_initializer: $ => seq(
      field('name', $.identifier),
      ':',
      field('value', $._expression),
    ),

    unit_expression: _ => seq('(', ')'),

    tuple_expression: $ => seq(
      '(',
      $._expression,
      // A tuple of one is written with its separator: `(n,)`.
      ',',
      optional(sepByTrailing($._expression, ',')),
      optional(','),
      ')',
    ),

    array_expression: $ => seq(
      '[',
      sepByTrailing($._expression, ','),
      ']',
    ),

    repeat_expression: $ => seq(
      '[',
      field('value', $._expression),
      ';',
      field('length', $.integer),
      ']',
    ),

    // Parentheses lift the ban a condition or a range end places on a
    // struct literal (grammar.md, "Expressions").
    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // A closure is an anonymous function: `(params) -> body`, with an
    // optional capture list naming the locals it sees. A bare parameter
    // list captures nothing, and `[]` says the same thing explicitly
    // (grammar.ebnf, "Expressions"). `mut` and `_` mirror declaration
    // patterns, and a parameter's type is optional.
    closure_expression: $ => seq(
      optional($.capture_list),
      field('parameters', $.closure_parameters),
      '->',
      field('body', $._expression),
    ),

    capture_list: $ => seq(
      '[',
      optional(sepByTrailing($.capture, ',')),
      ']',
    ),

    // One capture and how the closure takes it: a bare name takes the
    // value (a move, or a copy for a Copy type), `&name` borrows
    // shared, and `&mut name` borrows exclusively (grammar.ebnf,
    // "Expressions").
    capture: $ => seq(
      optional(seq('&', optional('mut'))),
      field('name', choice($.identifier, alias('self', $.self))),
    ),

    closure_parameters: $ => seq(
      '(',
      optional(sepByTrailing($.closure_parameter, ',')),
      ')',
    ),

    closure_parameter: $ => seq(
      optional(field('mut', 'mut')),
      field('name', choice($.identifier, $.wildcard_pattern)),
      optional(seq(':', field('type', $._type))),
    ),

    // A block that is an expression in its own right has no header to stay
    // on, so its brace is an ordinary one: `{ ... }` on a line of its own is
    // a block-valued statement. Every block that *does* have a header takes
    // `_block_lbrace` instead.
    block_expression: $ => seq('{', optional(blockBody($)), '}'),

    comp_block: $ => seq('comp', $.block),

    // `unsafe { ... }` opens the gate for the operations inside and its
    // value is the block's (ffi.md, "The gate").
    unsafe_block: $ => seq('unsafe', $.block),

    // Right-associative so that `ret (a)` reads the parenthesized
    // expression as the returned value rather than as a bare `ret`
    // followed by something the statement cannot start with.
    return_expression: $ => prec.right(seq(
      'ret',
      optional(field('value', $._expression)),
    )),

    break_expression: _ => 'break',

    continue_expression: _ => 'continue',

    // The `{` of `if`, `while`, `match`, and `else` stays on the header
    // line, which is what `_block_lbrace` enforces. A newline between the
    // consequence and its `else` is written out here rather than suppressed
    // by the scanner: telling a keyword from an identifier that starts the
    // same way means reading ahead, and the scanner has no way to put back
    // what it read.
    if_expression: $ => seq(
      'if',
      field('condition', $._condition),
      field('consequence', $.block),
      optional(seq(
        optional($._newline),
        'else',
        field('alternative', choice($.block, $.if_expression)),
      )),
    ),

    while_expression: $ => seq(
      'while',
      field('condition', $._condition),
      field('body', $.block),
    ),

    loop_expression: $ => seq('loop', field('body', $.block)),

    // A `for` head never reads a struct literal or a block as a range end,
    // so `for i in 0.. { }` is the open range and the loop body.
    for_expression: $ => seq(
      'for',
      field('pattern', $._pattern),
      'in',
      field('iterator', $._expression),
      field('body', $.block),
    ),

    match_expression: $ => seq(
      'match',
      field('scrutinee', $._expression),
      $._block_lbrace,
      itemList($, $.match_arm, $._arm_separator),
      '}',
    ),

    match_arm: $ => seq(
      field('pattern', $._pattern),
      '=>',
      field('value', $._expression),
    ),

    // `if cond block` or `if pattern := expr block`; `while` mirrors it.
    _condition: $ => choice(
      $.declaration_condition,
      $._expression,
    ),

    declaration_condition: $ => seq(
      field('pattern', $._pattern),
      ':=',
      field('value', $._expression),
    ),

    // Statements and blocks ------------------------------------------------

    block: $ => seq(
      $._block_lbrace,
      optional(blockBody($)),
      '}',
    ),

    _statement: $ => choice(
      $.declaration_statement,
      $.assignment_statement,
      $.expression_statement,
    ),

    // `mut` in declaration position folds into the pattern, so only `comp` is
    // a decl-level modifier.
    declaration_statement: $ => seq(
      optional(field('comp', 'comp')),
      field('pattern', $._pattern),
      optional(seq(':', field('type', $._type))),
      ':=',
      field('value', $._expression),
    ),

    // The parser reads the left side as an ordinary expression and leaves
    // the question of whether it names a place to the checker, so the
    // grammar does not restrict it either.
    assignment_statement: $ => seq(
      field('left', $._expression),
      field('operator', choice(
        '=', '+=', '-=', '*=', '/=', '%=', '**=', '&=', '|=', '^=', '<<=', '>>=',
      )),
      field('right', $._expression),
    ),

    // A block's last expression is its value rather than a statement, and
    // both readings parse, so the statement is the one that loses.
    expression_statement: $ => prec.dynamic(-1, seq(
      field('expression', $._expression),
      $._terminator,
    )),

    // A statement ends at a `;` or at the newline the lexer inserts. A
    // `match` arm accepts a run of `,` and `;` as its separator.
    _terminator: $ => choice(';', $._newline),
    _arm_separator: $ => choice(',', ';', $._newline),

    // Paths ----------------------------------------------------------------

    // `Name<T>` puts its type arguments straight after a segment and
    // `Name::<T>::member` after a `::`, which is the same thing with the
    // turbofish spelled out. Either may follow any segment, and either may
    // be followed by more segments (parser.cc, "parse_path").
    path: $ => seq(
      field('segment', $._path_segment),
      optional(field('type_arguments', $.type_arguments)),
      repeat(seq('::', choice(
        seq(
          field('segment', $._path_segment),
          optional(field('type_arguments', $.type_arguments)),
        ),
        field('type_arguments', $.type_arguments),
      ))),
    ),

    _path_segment: $ => choice(
      alias('package', $.package),
      alias('self', $.self),
      alias('super', $.super),
      alias('Self', $.self_type),
      $.identifier,
    ),

    // Literals -------------------------------------------------------------

    // `_` alone is the wildcard token and `_foo` is an identifier, which
    // is the same split the lexer makes: a leading underscore starts an
    // identifier unless nothing follows it.
    identifier: _ => /[A-Za-z][A-Za-z0-9_]*|_[A-Za-z0-9_]+/,

    _literal: $ => choice(
      $.integer,
      $.float,
      $.string,
      $.char,
      $.boolean,
    ),

    boolean: _ => choice('true', 'false'),

    // A `.` starts a fraction only when it is not the start of `..`, which
    // is what keeps `1..<2` a range and not a float. The fraction needs a
    // digit after the `.` for the same reason: `1..` must not reach for the
    // second `.`.
    integer: _ => token(choice(
      seq(/0[bB]/, repeat1(choice(/[01]/, '_')), optional(SUFFIX)),
      seq(/0[oO]/, repeat1(choice(/[0-7]/, '_')), optional(SUFFIX)),
      seq(/0[xX]/, repeat1(choice(HEX_DIGIT, '_')), optional(SUFFIX)),
      seq(
        repeat1(DECIMAL_DIGIT),
        optional(choice(
          seq('.', repeat1(DECIMAL_DIGIT)),
          seq(/[eE]/, optional(/[+-]/), repeat1(DECIMAL_DIGIT)),
        )),
        optional(SUFFIX),
      ),
    )),

    float: _ => token(seq(
      repeat1(DECIMAL_DIGIT),
      '.',
      repeat1(DECIMAL_DIGIT),
      optional(seq(/[eE]/, optional(/[+-]/), repeat1(DECIMAL_DIGIT))),
      optional(SUFFIX),
    )),

    string: $ => token(seq(
      '"',
      repeat(choice(
        /[^"\\\n]/,
        seq('\\', choice(
          /[ntr\\"']/,
          '0',
          seq('u', '{', repeat1(HEX_DIGIT), '}'),
        )),
      )),
      '"',
    )),

    char: $ => token(seq(
      "'",
      choice(
        /[^'\\\n]/,
        seq('\\', choice(/[ntr\\"']/, '0', seq('u', '{', repeat1(HEX_DIGIT), '}'))),
      ),
      "'",
    )),

    // Comments -------------------------------------------------------------

    line_comment: _ => token(seq('//', /[^\n]*/)),

    // `///` attaches to the following item. It is an extra, so it lands in
    // the tree as a sibling of what it documents rather than as a field of
    // it, which is all a query needs to colour it.
    doc_comment: _ => token(prec(1, seq('///', /[^\n]*/))),
  },
});

// A separator-separated list with an optional trailing separator.
function sepByTrailing(rule, separator) {
  return seq(rule, repeat(seq(separator, rule)), optional(separator));
}

// The statements of a block and the value it evaluates to. Spelled by a
// function rather than a hidden rule because it can match nothing at all, and
// tree-sitter has no rule that does.
function blockBody($) {
  return seq(
    itemList($, $._statement),
    // A block's value is its last expression, and `{}` evaluates to `()`.
    optional(field('value', $._expression)),
  );
}

// A list whose entries are separated by a token that may also appear in a
// run of its own. A separator standing where an entry goes is a no-op, which
// is what makes a stray one legal: `entry | separator`, repeated.
function itemList($, entry, separator) {
  const gap = separator === undefined ? $._terminator : separator;
  return repeat(choice(entry, gap));
}

// Every postfix operator's left operand.
function postfixOperand($) {
  return choice(
    $._primary_expression,
    $.call_expression,
    $.method_call_expression,
    $.field_expression,
    $.index_expression,
    $.try_expression,
  );
}
