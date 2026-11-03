// The exponent form keeps the value's own digits: the mantissa comes
// from the original magnitude and a power of ten the table holds, not
// the normalization loop's accumulated tenths. A subnormal scale is two
// factors, so neither passes the largest double and the digits carry
// instead of saturating.
fn main() -> i32 {
  println("{:.16f}", 1.7976931348623157e308)
  println("{:.17f}", 1.5e300)
  println("{:.17f}", 1.0e-20)
  println("{:.16f}", 1.4e-323)
  println("{:.14f}", 4.9406564584124654e-324)
  println("{:.15f}", 1.0e16)
  ret 0
}
