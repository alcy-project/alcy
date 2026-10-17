// The digits of the smallest signed value under wasm: the formatter
// takes the magnitude by a wrapping subtraction, so the checked build
// does not abort on a magnitude the signed range cannot hold
// (ADR-0066's rule that nothing is undefined).
fn main() {
  c: i64 := -9223372036854775808i64
  a: i32 := -2147483648i32
  println("{} {}", c, a)
}
