// `contains` compares each element with `==`.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  if !v.contains(&2) {
    ret 1
  }
  if v.contains(&9) {
    ret 2
  }
  mut s := Vec::<String>::new()
  s.push(format("x"))
  if !s.contains(&format("x")) {
    ret 3
  }
  if s.contains(&format("y")) {
    ret 4
  }
  mut e := Vec::<i32>::new()
  if e.contains(&1) {
    ret 5
  }
  ret 0
}
