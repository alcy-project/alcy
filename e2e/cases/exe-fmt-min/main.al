// The digits of the smallest value of a signed type: the formatter
// takes the magnitude by a wrapping subtraction, so the checked build
// does not abort on a value the signed range cannot hold (
// rule that nothing is undefined).
fn main() -> i32 {
  a: i32 := i32::MIN
  b: i8 := i8::MIN
  c: i64 := i64::MIN
  d: i16 := i16::MIN
  e: i32 := -2147483647i32
  println("{} {} {} {} {}", a, b, c, d, e)
  ret 0
}
