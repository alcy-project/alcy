# MVP Keywords and Tokens

Frozen set: the first list is usable in MVP, the second is reserved
(see `deferred.md`). Additions to either list require a specification
update.

## MVP

Declarations: `fn`, `struct`, `enum`, `impl`, `static`, `pub`,
`const`, `mut`, `use`

Control flow: `if`, `else`, `loop`, `while`, `break`, `continue`,
`ret`, `match`

Paths and casts: `package`, `self`, `super`, `Self`, `as`

Primitive types: `i8`, `i16`, `i32`, `i64`, `isize`, `u8`,
`u16`, `u32`, `u64`, `usize`, `f32`, `f64`, `bool`, `str`

## Reserved (parsed, rejected with guidance)

`async`, `await`, `union`, `register`, `extern`,
`unsafe`, `for`, `in`, `where`, `dyn`

## Bootstrap

`comp` (see `comp.md`), `intrinsic` (see `items.md`)

## Literals, operators, delimiters, comments

Literals: decimal, `0b`/`0o`/`0x` with type suffixes; `"..."`
strings; `'...'` characters (unresolved in MVP; `Char` lives
post-MVP, see `types.md`); `true`, `false`

Operators and delimiters: `+ - * / % **` `& | ^ ~ << >>` and
assignment forms; `:=` (declare), `=` (reassign); `!` `&&` `||`
`!=` `>` `<` `>=` `<=`; `->` `=>` `:` `::` `,` `.` `..` `..=` `..<`
`(` `)` `{` `}` `[` `]` `?` (error propagation), `_` (wildcard),
`#` (reserved for future attributes). `;` separates
multiple statements on one line only.

Comments: `//`, `/* */`, `///` (only one doc-comment style is MVP).
