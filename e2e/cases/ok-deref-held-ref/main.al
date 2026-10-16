// A write through a `&mut` the binding holds needs no `mut`.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  fr := v.first_mut().unwrap()
  *fr = 10
  lr := v.last_mut().unwrap()
  *lr = 20
  if *v.at(0).unwrap() != 10 {
    ret 1
  }
  if *v.at(1).unwrap() != 20 {
    ret 2
  }
  ret 0
}
