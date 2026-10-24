// A `&mut T` a call answers is a place through its deref.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  *v.first_mut().or_panic_with_message("expected a value") = 10
  *v.last_mut().or_panic_with_message("expected a value") = 20
  if *v.at(0).or_panic_with_message("expected a value") != 10 {
    ret 1
  }
  if *v.at(1).or_panic_with_message("expected a value") != 20 {
    ret 2
  }
  ret 0
}
