// The digits of the smallest value of a signed type: the formatter
// takes the magnitude by a wrapping subtraction, so the checked build
// does not abort on a value the signed range cannot hold (ADR-0066's
// rule that nothing is undefined).
fn main() -> i32 {
  a: i32 := -2147483648i32
  b: i8 := -128i8
  c: i64 := -9223372036854775808i64
  d: i16 := -32768i16
  e: i32 := -2147483647i32
  println("{} {} {} {} {}", a, b, c, d, e)
  ret 0
}
