// A negated literal is the literal's own sign: -128i8 is a value, not a
// subtraction, so a checked build never aborts where the source wrote
// one (ADR-0068).
fn main() -> i32 {
  x := -128i8
  if x == -128i8 {
    ret 0
  }
  ret 1
}
