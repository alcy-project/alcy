// Negating a float flips its sign bit. A subtraction from zero would
// answer +0.0 for +0.0, and the sign of a zero decides the sign of a
// quotient.

fn negate(value: f64) -> f64 {
  ret -value
}

fn main() -> i32 {
  zero := 0.0
  println("{}", -zero)
  println("{:.2f}", -zero)
  println("{}", 1.0 / -zero)
  println("{}", negate(0.0))
  println("{}", 1.0 / negate(0.0))
  negative := -0.0
  println("{}", -negative)
  println("{}", 1.0 / -negative)
  println("{}", -7.25)
  println("{}", -(-7.25))
  narrow := 0.0f32
  println("{}", -narrow)
  println("{}", 1.0f32 / -narrow)
  ret 0
}
