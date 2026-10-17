// A surrogate is not a scalar value, so the escape is rejected rather
// than encoded as the three bytes a raw value would spell.
fn main() -> i32 {
  s := "\u{D800}"
  ret 0
}
