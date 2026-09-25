# Formatting (Bootstrap)

`fmt::write` formats values into a caller-provided buffer. Literal
pieces expand to copy sequences at compile time; only per-argument
conversion runs at runtime.

## API

```alcy
pub struct WriteOutcome { written: usize, total: usize }

pub fn write(comp fmt: str, buf: &mut [u8; N], args: tuple) -> WriteOutcome
```

`N` and the tuple shape vary per call: the declaration names the
shape, and calls check customly (arity, buffer, convertibility)
because the language cannot name varying sizes and arities yet.

- `fmt` MUST be comp-known (a `comp` parameter); anything else is a
  compile-time error through the ordinary `comp` rules.
- `buf` bounds every write; overruns truncate silently. `written`
  counts stored bytes, `total` counts bytes the output would have
  taken untruncated; `total > buf.len` — equivalently
  `total > written` — detects truncation. Truncation never panics.
- `args` is a heterogeneous tuple, one element per placeholder.

## Placeholders

- `{}` formats the next argument in order. `{{` and `}}` emit literal
  braces. Anything else inside braces (`{0}`, `{x}`, unclosed `{`,
  lone `}`) is a compile-time error.
- Arity mismatch (placeholder count vs. tuple arity) is a
  compile-time error.

## Convertible types

- Integers format as decimal, `bool` as `true`/`false`, `str` as its
  bytes. Any other argument type is a compile-time error.
- Integer conversion is exact; truncation applies to the byte stream
  only, never to digits.

## Expansion

- Calls to `fmt::write` expand at lowering time to bounded copies
  per piece. The declared body never executes; reaching it aborts
  through `panic`.
- Only the core prelude item expands: a local `write` shadows the
  prelude through the ordinary name rules, and calls to it lower
  ordinarily without expansion.

## Deferred

- Numbered/indexed placeholders and format specifiers.
- Float formatting, user-defined formattability, width/precision.
- A `format` convenience returning allocated strings (needs `alloc`).
