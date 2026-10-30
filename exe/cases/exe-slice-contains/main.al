// A slice answers membership the way `==` compares elements.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  s := v.as_slice()
  if !s.contains(&2) {
    ret 1
  }
  if s.contains(&9) {
    ret 2
  }
  a := [1, 2, 3]
  if !a.as_slice().contains(&3) {
    ret 3
  }
  ret 0
}
