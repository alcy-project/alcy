# ADR-0134: A decimal float reads once

- Subject: the language
- Status: Accepted
- Date: 2026-11-01

## Context

`f64::parse` folded a decimal's digits into one `u64` numerator and a
power-of-ten scale, then converted the integer part and the fraction
apart: `(numerator / scale) as f64 + (numerator % scale) as f64 / (scale as f64)`.
Two roundings stand in that expression - the quotient's conversion and
the division's - so a numerator past 2^53 answers a value one step away
from the decimal it spells. The compiler converts a literal with the
host's `strtod`, which rounds once, and the two spellings of one
decimal are one value to a reader: `"0.9876543210987654321"` as text
read as 0.98765432109876550 where the same digits as a literal read as
0.98765432109876539, and `"0.3333333333333333333"` and
`"0.6666666666666666667"` disagreed the same way.

The standard library may not ask the host: `lib/std` calls nothing
outside the intrinsics the compiler provides, and a freestanding target
has no `strtod`.

## Decision

**A decimal float is read with exact integer arithmetic and rounded
once, to the nearest `f64`, ties to even - the value the same digits
carry as a literal.**

- The digits form a `u64` numerator with the point removed and a
  `u64` scale, as before, and the guards that refuse a numerator or
  scale past a `u64` stay (`None`).
- A numerator at most 2^53 is converted and divided once: both operands
  are exact, and IEEE division of exact operands is already the nearest
  value.
- A longer numerator is assembled into a 54-bit significand: the
  quotient's leading bits, then one fractional bit per step with the
  remainder doubled inside `u64` (the carry reads the bit the shift
  drops, and the subtraction wraps), while every bit dropped past the
  last kept one is OR-ed into a sticky flag.
- One guard bit rounds the 54-bit significand to nearest, a tie going
  to the even significand.
- The binary exponent is applied by multiplying or dividing by two,
  which moves the point without a rounding, and the sign is applied
  last, so `"-0.0"` keeps it.

## Consequences

- Text and a literal of the same digits agree bit for bit, and
  `e2e/cases/ok-str-parse-f64/main.al` pins the shapes: three 19-digit
  fractions, the ties `"9007199254740993.0"` (down to 2^53) and
  `"9007199254740995.0"` (up to 2^53 + 4), the twenty-digit integer,
  and `"-0.0"`.
- A zero arrives with a sign now, so `Display` reads the sign of a
  float through its reciprocal rather than a comparison
  (`docs/spec/fmt.md`): `-0.0` prints as `-0`.
- A decimal whose digits and factors of five pass what a `u64` holds is
  still refused rather than approximated (`"18446744073709551616.0"`,
  `"1e-400"`), which is the work `docs/backlog.md` records for a wider
  word.

## Amendments

**A literal reads once at its own type too.** The compiler read an
`f32` literal with `strtod` and narrowed the double, so the digits
rounded twice and the second rounding could land on a neighbour of the
value they name: `1.0000000596046447753906250000000000000001f32` is a
hair above the midpoint between `1.0` and `1 + 2^-23` and answers
`1 + 2^-23`, but the double landed on the midpoint and tied to the even
significand, `1.0`; `3.4028235677973366e38f32` is just under the
overflow threshold and answers the largest finite `f32`, but the double
rounded past the threshold and answered infinity. An `f32` literal now
reads with `strtof`, which rounds the digits once to `f32`, and the
rule above holds for either float type.

**A decimal place folds fives into the scale too.** A positive power
folded into the numerator as factors of five, the twos riding the binary
exponent, while a decimal place and a negative power folded a whole ten
into the scale - so a `u64` held ten nineteen times where it holds five
twenty-seven, and `f64::parse("3.9577e-16")` answered `None` although
the literal `3.9577e-16` compiles to a value. A decimal place and a
negative power now fold a factor of five into the scale and a factor of
two onto the binary exponent, as their positive counterpart does. The
quotient is divided once only while both of its operands are exact, so
that path now asks that the scale also be at most 2^53; a longer scale
goes through the significand assembly, which reads the same integers
without a rounding of its own.
