// A decimal float folds its digits into one numerator, and the quotient
// is rounded once to the nearest value. An exponent scales the digits by
// a power of ten, which lands in the numerator and in the binary exponent
// so the rounding stays single.
fn main() -> i32 {
  if f64::parse("3.5").or_panic_with_message("expected a value") != 3.5 {
    ret 1
  }
  if f64::parse("0.5").or_panic_with_message("expected a value") != 0.5 {
    ret 2
  }
  if f64::parse("-2.25").or_panic_with_message("expected a value") != 0.0f64 - 2.25 {
    ret 3
  }
  if f64::parse("3.14").or_panic_with_message("expected a value") != 3.14 {
    ret 4
  }
  if f64::parse("0.1").or_panic_with_message("expected a value") != 0.1 {
    ret 5
  }
  if f64::parse("1").or_panic_with_message("expected a value") != 1.0 {
    ret 6
  }
  if f64::parse("1.").or_panic_with_message("expected a value") != 1.0 {
    ret 7
  }
  if f64::parse(".5").or_panic_with_message("expected a value") != 0.5 {
    ret 8
  }
  if f64::parse("+0.25").or_panic_with_message("expected a value") != 0.25 {
    ret 9
  }
  if f64::parse("x").is_some() {
    ret 10
  }
  if f64::parse("").is_some() {
    ret 11
  }
  if f64::parse("1.2.3").is_some() {
    ret 12
  }
  s := format("2.5")
  if f64::parse(s.as_str()).or_panic_with_message("expected a value") != 2.5 {
    ret 13
  }
  // A numerator past the significand keeps the bit a second rounding
  // would lose.
  if f64::parse("0.9876543210987654321").or_panic_with_message("expected a value") != 0.9876543210987654321 {
    ret 14
  }
  if f64::parse("0.3333333333333333333").or_panic_with_message("expected a value") != 0.3333333333333333333 {
    ret 15
  }
  if f64::parse("0.6666666666666666667").or_panic_with_message("expected a value") != 0.6666666666666666667 {
    ret 16
  }
  if f64::parse("1.7976931348623157").or_panic_with_message("expected a value") != 1.7976931348623157 {
    ret 17
  }
  // Exactly between two values, and the even significand wins both ways:
  // 2^53 + 1 falls back to 2^53, and 2^53 + 3 rises to 2^53 + 4.
  if f64::parse("9007199254740993.0").or_panic_with_message("expected a value") != 9007199254740993.0 {
    ret 18
  }
  if f64::parse("9007199254740995.0").or_panic_with_message("expected a value") != 9007199254740995.0 {
    ret 19
  }
  if f64::parse("18446744073709551615").or_panic_with_message("expected a value") != 18446744073709551615.0 {
    ret 20
  }
  if f64::parse("922337203685477580.7").or_panic_with_message("expected a value") != 922337203685477580.7 {
    ret 25
  }
  // A zero keeps the sign the text carries.
  zero := f64::parse("-0.0").or_panic_with_message("expected a value")
  if zero != 0.0 {
    ret 21
  }
  if 1.0f64 / zero > 0.0f64 {
    ret 22
  }
  // The digits pass the numerator a u64 holds, and the read refuses
  // rather than answering an approximation.
  if f64::parse("18446744073709551616.0").is_some() {
    ret 23
  }
  // Twenty decimal places fit the scale as factors of five, which a u64
  // holds twenty-seven times, so the digits read like the literal.
  if f64::parse("0.00000000000000000001").or_panic_with_message("expected a value") != 0.00000000000000000001 {
    ret 24
  }
  // The exponent form the formatter writes reads back to the same value.
  if f64::parse("1e10").or_panic_with_message("expected a value") != 1.0e10 {
    ret 26
  }
  if f64::parse("2.5e-3").or_panic_with_message("expected a value") != 2.5e-3 {
    ret 27
  }
  if f64::parse("6.02E23").or_panic_with_message("expected a value") != 6.02e23 {
    ret 28
  }
  if f64::parse("+2e+3").or_panic_with_message("expected a value") != 2000.0 {
    ret 29
  }
  if f64::parse("-1.5e-5").or_panic_with_message("expected a value") != 0.0f64 - 1.5e-5 {
    ret 30
  }
  if f64::parse("9007199254740993e0").or_panic_with_message("expected a value") != 9007199254740993.0 {
    ret 31
  }
  if f64::parse("0.9876543210987654321e0").or_panic_with_message("expected a value") != 0.9876543210987654321 {
    ret 32
  }
  if f64::parse("0e400").or_panic_with_message("expected a value") != 0.0 {
    ret 33
  }
  round_trip := format("{}", 6.02e23)
  if f64::parse(round_trip.as_str()).or_panic_with_message("expected a value") != 6.02e23 {
    ret 34
  }
  // An exponent needs digits, and a power of ten past what the numerator
  // and the scale hold is refused the way a long fraction is.
  if f64::parse("1e").is_some() {
    ret 35
  }
  if f64::parse("1e+").is_some() {
    ret 36
  }
  if f64::parse("e5").is_some() {
    ret 37
  }
  if f64::parse("1e1.5").is_some() {
    ret 38
  }
  if f64::parse("1e400").is_some() {
    ret 39
  }
  if f64::parse("1e-400").is_some() {
    ret 40
  }
  // A negative power folds fives into the scale the way a positive one
  // folds them into the numerator, so a small magnitude reads.
  if f64::parse("1e-27").or_panic_with_message("expected a value") != 1e-27 {
    ret 41
  }
  if f64::parse("-3.9577e-16").or_panic_with_message("expected a value") != 0.0f64 - 3.9577e-16 {
    ret 42
  }
  if f64::parse("0.6666666666666666667e-6").or_panic_with_message("expected a value") != 0.6666666666666666667e-6 {
    ret 43
  }
  // The scale's twenty-eighth five passes the word the read works in.
  if f64::parse("1e-28").is_some() {
    ret 44
  }
  ret 0
}
