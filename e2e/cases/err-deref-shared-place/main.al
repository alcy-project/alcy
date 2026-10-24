// A shared reference is not a place to write.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  *v.first().or_panic_with_message("expected a value") = 10
  ret 0
}
