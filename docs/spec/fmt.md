# Formatting (Bootstrap)

`fmt::write` formats values into a caller-provided buffer. Literal
pieces expand to copy sequences at compile time; only per-argument
conversion runs at runtime.

## API

```text
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
- `{:.Nf}` fixes the fractional digits of a float argument: exactly
  `N` after the point, rounded half-up, no trimming, and no point when
  `N` is zero. The `f` names the conversion explicitly and may be
  omitted - `{:.N}` is the same piece, and the analyzer warns in favor
  of the explicit spelling. `N` is
  at most 17, and a precision on any other type is a compile-time
  error. The magnitude cutoffs still apply: outside the fixed range
  the exponent form carries the fixed count.
- Arity mismatch (placeholder count vs. tuple arity) is a
  compile-time error.

## Convertible types

- Integers format as decimal, `bool` as `true`/`false`, `str` as its
  bytes, and `f32`/`f64` as decimal magnitudes. Any other argument
  type is a compile-time error unless it implements `Display`.
- A nominal type formats through `fmt`'s `Display` spec when it
  implements it: `fn display(self: &Self, out: &mut [u8]) -> usize`
  writes the prefix of its rendering that fits into `out` and returns
  the total length, which is how the expansion learns the size for a
  growable buffer (the measure pass calls it with an empty slice). One
  placeholder may run the method twice, so it must not have effects
  beyond `out`. A nominal without the implementation is a compile-time
  error, and the spec is sealed like the operator specs, so a package
  outside the standard library's suite lists `Display` in its own
  `[spec] implement` (see `modules.md`).
- Integer conversion is exact; truncation applies to the byte stream
  only, never to digits.
- A float prints up to six fractional digits of its exact fraction,
  rounded half-up, with trailing zeros trimmed and the point dropped
  when nothing survives (`3.0` prints as `3`). A magnitude below 1e-4
  or at least 1e16 prints its mantissa in [1, 10) followed by `e` and
  the decimal exponent (`1.5e-5`, `1e20`), so every finite value is
  printable. `NaN` and the infinities print as `NaN`, `inf`, and
  `-inf`.
- Float conversion is exact to the binary value's precision, about
  sixteen significant digits; digits past that are zero rather than
  rounded.

## Expansion

- Calls to `fmt::write` expand at lowering time to bounded copies
  per piece. The declared body never executes; reaching it aborts
  through `panic`.
- Only the `fmt` package's item expands: a local `write` shadows the
  package's through the ordinary name rules, and calls to it lower
  ordinarily without expansion.

## Deferred

- Numbered/indexed placeholders; width, alignment, and conversions
  other than the implicit decimal one and `f`.
- User-defined formattability.
- A single-pass growable expansion for `format`: today it measures the
  output before reserving the heap buffer, so every value formats
  twice, a `Display` implementation included (a precision does not
  change that).
