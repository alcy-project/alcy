// A \u{...} escape must name a Unicode scalar value: past the last one
// the encoder would drop it, changing the string silently.
fn main() -> i32 {
  s := "x\u{110000}y"
  ret 0
}
