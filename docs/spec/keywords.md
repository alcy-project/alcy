# MVP Keywords and Tokens

Frozen set: the first list is usable in MVP, the second is reserved
(see `deferred.md`). Additions to either list require a specification
update.

## MVP

Declarations: `fn`, `struct`, `enum`, `impl`, `spec`, `static`, `pub`,
`const`, `mut`, `use`

Control flow: `if`, `else`, `loop`, `while`, `for`, `in`, `break`,
`continue`, `ret`, `match`

Paths and casts: `package`, `self`, `super`, `Self`, `as`

Primitive types: `i8`, `i16`, `i32`, `i64`, `isize`, `u8`,
`u16`, `u32`, `u64`, `usize`, `f32`, `f64`, `bool`, `str`

## Reserved (parsed, rejected with guidance)

`async`, `await`, `union`, `register`,
`where`, `dyn`

A reserved word is read past rather than refused: the diagnostic names it
and parsing continues, so a program using one is a program with an error
in it rather than one that fails to read. `i128` and `u128` are reserved
the same way; the lexer knows them so that naming one produces a
diagnostic about the type rather than about the token, and `types.md`
records them as deferred. `f16` is not reserved yet: it lexes as an
identifier and reports an unknown name.

## Contextual

`comp`, `intrinsic`, `spec`, `for`, and `in` are keywords rather than
reserved words, because each means something in one position and nothing
outside it: `comp` in a signature, a declaration, or a block;
`intrinsic` and `spec` in item position; `for` in a loop head and in an
`impl` header; `in` in a `for` head. Using one outside its position is an
ordinary parse error, not a reserved-name diagnostic.

`for` introduces a loop and separates the spec from the target in
`impl S for T` headers (see `items.md`); `in` is read only between a
`for` pattern and its head (see `control.md`).

## Bootstrap

`comp` (see `comp.md`), `intrinsic` (see `items.md`), `unsafe` and
`extern` (see `ffi.md`)

## Literals, operators, delimiters, comments

Literals: decimal, `0b`/`0o`/`0x` with type suffixes; `"..."`
strings; `'...'` characters (unresolved in MVP; `Char` lives
post-MVP, see `types.md`); `true`, `false`

Operators and delimiters: `+ - * / % **` `& | ^ ~ << >>` and
assignment forms; `:=` (declare), `=` (reassign); `!` `&&` `||`
`!=` `>` `<` `>=` `<=`; `->` `=>` `:` `::` `,` `.` `..` `..=` `..<`
(a range end is spelled `..=` or `..<`; bare `..` names no end, see
`types.md`) `(` `)` `{` `}` `[` `]` `?` (error propagation),
`_` (wildcard), `#` (reserved for future attributes). `;` separates
multiple statements on one line only.

Comments: `//`, `/* */`, `///` (only one doc-comment style is MVP).
