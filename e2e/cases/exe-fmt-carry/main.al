// Rounding a fraction half-up carries into the integer part, and the
// trim that follows the default form's six digits drops the zeros the
// carry leaves: `0.9999999` prints `1`, and `9.9999999` prints `10`.
// The fixed form keeps its count across the same carry (`{:.2f}` of
// `0.999` prints `1.00`), a negative zero keeps its sign in both forms,
// and `{:.0f}` rounds away from zero on either side.
fn main() -> i32 {
  println("{}", 0.9999999f64)
  println("{}", 9.9999999f64)
  println("{}", -0.9999999f64)
  println("{}", 0.99999949f64)
  println("{}", 1.0000001f64)
  println("{:.2f}", 0.999f64)
  println("{:.2f}", 9.996f64)
  println("{:.0f}", 0.9f64)
  println("{:.0f}", -2.5f64)
  println("{:.0f}", -0.5f64)
  println("{:.3f}", 0.0005f64)
  println("{:.3f}", 99.9999999f64)
  println("{:.4f}", 1234.56785f64)
  println("{}", 1e15f64)
  println("{}", 9007199254740992.0f64)
  println("{}", 9999999999999998.0f64)
  println("{}", -0.0f64)
  println("{:.2f}", -0.0f64)
  println("{:.17f}", -0.0f64)
  println("{:.17f}", 0.0f64)
  println("{:.17f}", 0.9999999999999999f64)
  println("{}", -0.00009f64)
  ret 0
}
