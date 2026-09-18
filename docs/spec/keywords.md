# MVP Keywords and Tokens

Frozen set: the first list is usable in MVP, the second is reserved
(see `deferred.md`). Additions to either list require a specification
update.

## MVP

Declarations: `fn`, `struct`, `enum`, `mod`, `impl`, `static`, `pub`,
`const`, `mut`, `use`

Control flow: `if`, `else`, `loop`, `while`, `break`, `continue`,
`ret`, `match`

Paths and casts: `package`, `self`, `super`, `Self`, `as`

Primitive types: `i8`, `i16`, `i32`, `i64`, `i128`, `isize`, `u8`,
`u16`, `u32`, `u64`, `u128`, `usize`, `f32`, `f64`, `bool`, `str`

## Reserved (parsed, rejected with guidance)

`async`, `await`, `union`, `register`, `comp`, `extern`,
`unsafe`, `for`, `in`, `where`, `dyn`

## Literals, operators, delimiters, comments

Literals: decimal, `0b`/`0o`/`0x` with type suffixes; `"..."`
strings; `'...'` characters (resolved to core `Char`; see
`types.md`); `true`, `false`

Operators and delimiters: `+ - * / % **` `& | ^ ~ << >>` and
assignment forms; `:=` (declare), `=` (reassign); `!` `&&` `||`
`!=` `>` `<` `>=` `<=`; `->` `=>` `:` `::` `,` `.` `..` `..=` `..<`
`(` `)` `{` `}` `[` `]` `?` (error propagation), `_` (wildcard),
`#` (reserved for future attributes). `;` separates
multiple statements on one line only.

Comments: `//`, `/* */`, `///` (only one doc-comment style is MVP).
