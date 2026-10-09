# Formatting (Bootstrap)

`fmt::write` formats values into a caller-provided buffer. Literal
pieces expand to copy sequences at compile time; only per-argument
conversion runs at runtime.

## API

```alcy
pub struct WriteOutcome { pub written: usize, pub total: usize }

pub fn write(comp fmt: str, buf: &mut [u8; N], args: ..) -> WriteOutcome
```

`N` varies per call: the declaration names a placeholder shape, and
calls check customly (buffer and convertibility) because the language
cannot name a varying size yet. `args: ..` collects the call's
remaining arguments, one per placeholder.

- `fmt` MUST be comp-known (a `comp` parameter); anything else is a
  compile-time error through the ordinary `comp` rules.
- `buf` bounds every write; overruns truncate silently. `written`
  counts stored bytes, `total` counts bytes the output would have
  taken untruncated; `total > buf.len` - equivalently
  `total > written` - detects truncation. Truncation never panics.
- The collected arguments are one per placeholder, in order.

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
- Only the `fmt` package's item expands: a local `write` shadows the
  package's through the ordinary name rules, and calls to it lower
  ordinarily without expansion.

## Deferred

- Numbered/indexed placeholders and format specifiers.
- Float formatting, user-defined formattability, width/precision.
- A single-pass growable expansion for `format`: today it measures the
  output before reserving the heap buffer, so every value formats
  twice.
