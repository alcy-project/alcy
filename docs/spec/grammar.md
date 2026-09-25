# MVP Grammar

Notation is EBNF-ish: `A*` repetition, `A?` optional, `A | B`
alternation, `"x"` literal tokens. Newlines are significant except
inside brackets (see statements). Longest-match lexing applies
throughout. This grammar covers MVP only; reserved forms are
rejected with guidance diagnostics.

## Lexical notes

- `..=`, `..<` are single tokens. After an integer literal, `..`
  starts a range operator, never a float fraction (`1..2` is
  `1 .. 2`); `1. < 2` keeps float-then-compare.
- Newlines are significant: outside brackets, a newline is lexed as
  `;` when the preceding token ends a statement (identifiers,
  literals, `)`, `]`, `}`, `break`, `continue`, `ret`) unless the
  next token continues the construct (`else`, `,`, closers, `.`, or
  end of file). A line therefore continues only in operator-led,
  bracket-open, or continuation positions.
- Block openers stay on their header line: the `{` of `if`, `while`,
  `match`, `fn`, `impl`, and `else` MUST NOT start on a following
  line (Go-style). `} else {` stays on one line.
- Block comments nest. `///` attaches to the following item.
- Integer literals accept `_` separators between digits. Suffixes
  (`42i32`, `1.5f64`) select the type; unsuffixed integers default
  to `i32`, floats to `f64`.
- String escapes: `\n \t \r \\ \" \0 \u{HEX}`. `'x'` character
  syntax exists but resolves only with core (see `types.md`).

## Items

```
item      := vis? (fn_item | struct_item | enum_item | impl_item |
                   static_item | const_item | use_item)
vis       := "pub"
fn_item   := "fn" ident "(" params ")" ("->" type)? block
            # omitted return type means "()"
params    := (pattern ":" type ("," pattern ":" type)* ","?)?
struct_item := "struct" ident "{" field ("," field)* ","? "}"
field     := ident ":" type
enum_item := "enum" ident ("<" ident ("," ident)* ">")? "{" variant ("," variant)* ","? "}"
variant   := ident | ident "(" (type ("," type)*)? ")"
impl_item := "impl" ("<" ident ("," ident)* ">")? type "{" fn_item* "}"
            # methods take self, &self, or &mut self first; other
            # functions in the block are associated functions

static_item := "static" ident ":" type "=" expr
const_item  := "const" ident ":" type "=" literal_expr
literal_expr := literal   # MVP const items admit literals only
use_item  := ("pub")? "use" path ("as" ident)? ";"
```

Methods take explicit receivers: `self`, `&self`, `&mut self`.
`Self` denotes the implementing type inside `impl` blocks.

## Types

```
type := primitive | "()" | "!" | "str" | tuple_type | array_type | path_type | ref_type
primitive := integer | float | "bool"
tuple_type := "(" type ("," type)+ ","? ")"
array_type := "[" type ";" integer "]"   # fixed-size array, decimal length
ref_type  := "&" type | "&" "mut" type
path_type := path ("<" type ("," type)* ">")?   # generic enums only
            # Closing ">>" splits into two ">" (dangling halves error).
```

`()` is the unit type. `!` is the never type and coerces to any type.
Tuple types are structural and concrete (no polymorphism in MVP).

## Patterns (shared by declarations and `match`)

```
pattern := "_" | ident | "mut" ident | literal
         | path "(" pattern ("," pattern)* ")"   # tuple variants, tuples
         | path "{" field_pat ("," field_pat)* "}"  # struct patterns
         | "&" pattern | "&" "mut" pattern
         | pattern "|" pattern                    # or-patterns
field_pat := ident | ident ":" pattern
```

Declaration left-hand sides use this grammar with `:=`
(`(c, _) := ...`, `_ := ...`). Range patterns are deferred.

## Expressions (lowest to highest precedence)

```
expr      := range_expr
range_expr := or_expr (("..=" | "..<") or_expr?)? | ".." or_expr?
            # bare ".." with an endpoint is rejected: mark "=" or "<".
            # (range expressions type-check once Range types exist;
            #  see deferred.md)
or_expr   := and_expr ("||" and_expr)*
and_expr  := cmp_expr ("&&" cmp_expr)*
cmp_expr  := bit_or_expr (("==" | "!=" | ">" | "<" | ">=" | "<=") bit_or_expr)?
            # no chaining
bit_or_expr := bit_xor_expr ("|" bit_xor_expr)*
bit_xor_expr := bit_and_expr ("^" bit_and_expr)*
bit_and_expr := shift_expr ("&" shift_expr)*
shift_expr := add_expr ("<<" | ">>" add_expr)*
add_expr  := mul_expr (("+" | "-") mul_expr)*
mul_expr  := pow_expr (("*" | "/" | "%") pow_expr)*
pow_expr  := cast_expr ("**" pow_expr)?      # right associative
cast_expr := unary_expr ("as" type)?
unary_expr := ("-" | "!" | "~" | "&" | "&" "mut") unary_expr | question_expr
question_expr := postfix_expr "?"?
postfix_expr := primary (call | field | method | index)*
call      := "(" (expr ("," expr)*)? ")"
field     := "." ident | "." integer        # tuple ".0" access
method    := "." ident "(" (expr ("," expr)*)? ")"
            # the receiver is the postfix base, not listed
index     := "[" expr "]"                    # builtin fixed-array index
primary   := literal | path | struct_expr | tuple_expr | array_expr
             | paren_expr | block_like
paren_expr  := "(" expr ")"
struct_expr := path "{" field_init ("," field_init)* ","? (".." expr)? "}"
field_init  := ident ":" expr
tuple_expr  := "(" expr "," expr ("," expr)* ","? ")" | "()"
array_expr  := "[" expr ("," expr)* ","? "]" | "[" expr ";" integer "]"
              # list literal, or repeat with a decimal count
block_like  := block | if_expr | match_expr | loop_expr | while_expr
```

- `?` binds tighter than all binary operators.
- Unary `*` (dereference) does not exist: no raw pointers in MVP.
- Array indexing is builtin with panic-on-out-of-bounds semantics.
- Calls to `panic(...)` diverge with type `!`.
- Closures do not exist; `||` is logical-or only.
- In statement position `Path {` opens a struct expression. After
  `if`/`while`/`match` conditions and `else`, `{` always opens a
  block: parenthesize conditions containing struct literals.
- `match` scrutinees and `if`/`while` conditions never parse a
  struct literal directly (the `{` belongs to the body); this keeps
  `match x {` and `if c {` unambiguous without lookahead.

## Statements and blocks

```
block  := "{" statement* expr? "}"
statement := decl_stmt | expr_stmt
decl_stmt  := pattern (":" type)? ":=" expr
reassign   := place "=" expr           # existing mut binding only
place      := path (field | index)*    # no dereference in MVP
expr_stmt  := expr
```

- Newlines terminate statements; `;` separates multiple statements on
  one line only (a trailing `;` is an allowed no-op). Stray `;` are
  skipped in item, block, and arm lists; `match` arms accept runs of
  `,` and `;` as separators.
- A block's value is its last expression; `{}` evaluates to `()`.
- `if cond block (else block)?` and `if pattern := expr block
  (else block)?`; `while` mirrors `if` (both accept
  pattern-declarations).
- `match scrutinee "{" (pattern "=>" expr ",")* "}"`
- `loop block`, `while cond block`; `break`, `continue` (unlabeled);
  loops evaluate to `()`.
- `ret expr?` returns early from the enclosing function.

## Modules and paths

```
path := ("package" | "self" | "super" | "<dep>" | ident)
        ("::" ident)*
```

Module declarations come exclusively from `alcy.toml` `[modules]`.
Files not included in `include` and unreachable from declared modules
warn (see `modules.md`).
