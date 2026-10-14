// A `&mut self` method needs a `mut` binding when the receiver owns
// the value.
fn main() -> i32 {
  s := String::new()
  s.push(65u8)
  ret 0
}
