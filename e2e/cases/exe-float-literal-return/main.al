// A literal returned as a float must travel through the floating-point
// return register, not the integer one that shares its number.
fn literal() -> f64 {
  ret 1.5f64
}

fn main() {
  println("{}", literal())
}
