// The digits of the smallest signed value under wasm: the formatter
// takes the magnitude by a wrapping subtraction, so the checked build
// does not abort on a magnitude the signed range cannot hold
// (rule that nothing is undefined).
fn main() {
  c: i64 := i64::MIN
  a: i32 := i32::MIN
  println("{} {}", c, a)
}
