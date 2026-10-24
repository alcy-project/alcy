// `extend` consumes another vector and moves its elements on.
fn main() -> i32 {
  mut a := Vec::<i32>::new()
  a.push(1)
  a.push(2)
  mut b := Vec::<i32>::new()
  b.push(3)
  b.push(4)
  a.extend(b)
  if a.len() != 4 {
    ret 1
  }
  if *a.at(0).or_panic_with_message("expected a value") != 1 {
    ret 2
  }
  if *a.at(3).or_panic_with_message("expected a value") != 4 {
    ret 3
  }
  mut s := Vec::<String>::new()
  s.push(format("a"))
  mut t := Vec::<String>::new()
  t.push(format("b"))
  s.extend(t)
  if s.len() != 2 {
    ret 4
  }
  if s.at(1).or_panic_with_message("expected a value").as_str() != "b" {
    ret 5
  }
  ret 0
}
