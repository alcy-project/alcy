// A write through a `&mut` the binding holds needs no `mut`.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  fr := v.first_mut().or_panic_with_message("expected a value")
  *fr = 10
  lr := v.last_mut().or_panic_with_message("expected a value")
  *lr = 20
  if *v.at(0).or_panic_with_message("expected a value") != 10 {
    ret 1
  }
  if *v.at(1).or_panic_with_message("expected a value") != 20 {
    ret 2
  }
  ret 0
}
