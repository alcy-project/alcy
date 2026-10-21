// The exponent form keeps the value's own digits: the mantissa comes
// from the original magnitude and one rounded power of ten, not the
// normalization loop's accumulated tenths. The fixed form below 1e16 is
// unchanged; this pins the large end and the subnormals.
fn main() -> i32 {
  println("{:.16f}", 1.7976931348623157e308)
  println("{:.17f}", 1.5e300)
  println("{:.17f}", 1.0e-20)
  println("{:.17f}", 4.9406564584124654e-324)
  println("{:.15f}", 1.0e16)
  ret 0
}
