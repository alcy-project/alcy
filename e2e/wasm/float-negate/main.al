// Negating a float flips its sign bit, so a zero keeps the sign the
// sign bit carries and a quotient reads it.
fn main() {
  zero := 0.0
  println("{}", -zero)
  println("{}", 1.0 / -zero)
  negative := -0.0
  println("{}", -negative)
  println("{}", 1.0 / -negative)
  narrow := 0.0f32
  println("{}", -narrow)
  println("{}", 1.0f32 / -narrow)
}
