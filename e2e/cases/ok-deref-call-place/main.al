// A `&mut T` a call answers is a place through its deref.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  *v.first_mut().unwrap() = 10
  *v.last_mut().unwrap() = 20
  if *v.at(0).unwrap() != 10 {
    ret 1
  }
  if *v.at(1).unwrap() != 20 {
    ret 2
  }
  ret 0
}
