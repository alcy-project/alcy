// A `&mut self` method needs a `mut` binding when the receiver owns
// the value.
fn main() -> i32 {
  s := String::new()
  s.push_str("A")
  ret 0
}
