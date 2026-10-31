// Digits come from the value rather than from a product of ten: the
// fixed form converts exactly, and the exponent form's scale is a power
// of ten the table holds, so a magnitude past the largest double (a
// subnormal, or one under 1e-4) keeps its leading digits instead of
// running into an infinity.
fn main() -> i32 {
  tenth := 0.1f64
  third := 1.0f64 / 3.0f64
  smallest := 5e-324f64
  sparse := 1.4e-323f64
  least := 2.2250738585072014e-308f64
  println("{:.17f}", tenth)
  println("{:.17f}", third)
  println("{}", smallest)
  println("{:.16f}", sparse)
  println("{:.16f}", least)
  ret 0i32
}
