// The fixed-point digits are the value's own: the fraction's digits come
// from its exact binary form, so the seventeenth place is right where
// repeated multiplication by ten lost it (ADR-0066's rule that nothing
// is undefined, applied to the formatter).
fn main() -> i32 {
  println("{:.17f}", 0.1)
  println("{:.17f}", 0.7)
  println("{:.17f}", 1.0 / 3.0)
  println("{:.17f}", 0.125)
  println("{:.17f}", 2.675)
  println("{:.16f} {:.15f}", 0.1, 0.1)
  println("{:.0f} {:.0f} {:.0f}", 0.5, 1.5, 2.5)
  println("{:.17f}", 1.0)
  println("{:.17f}", 0.3)
  println("{:.17f}", 0.05)
  ret 0
}
