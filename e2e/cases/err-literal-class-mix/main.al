// A position of one class does not claim a literal of the other: an
// unsuffixed float beside an integer is a float, and an integer literal
// neither becomes a float nor makes a float operand an integer.
fn main() -> i32 {
  unspecified := 1.5 * 2
  annotated: f32 := 1 * 2.0
  suffixed: f32 := 1.5f32 * 2.0f64
  ret 0
}
