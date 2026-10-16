// Mutable end access writes through the reference; the binding must be
// `mut` for the write to reach it.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  mut fr := v.first_mut().unwrap()
  *fr = 10
  mut lr := v.last_mut().unwrap()
  *lr = 20
  if *v.at(0).unwrap() != 10 {
    ret 1
  }
  if *v.at(1).unwrap() != 20 {
    ret 2
  }
  mut e := Vec::<i32>::new()
  if e.first_mut().is_some() {
    ret 3
  }
  if e.last_mut().is_some() {
    ret 4
  }
  ret 0
}
