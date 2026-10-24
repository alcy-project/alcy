// A view used as the call's own argument is live at the activation:
// the reservation becomes the loan while the shared borrow still
// reads through it, so the call reports rather than invalidating the
// read it was handed.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  held := v.at(0)
  v.push(*held.or_panic_with_message("expected a value"))
  ret 0
}
