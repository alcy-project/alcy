// `insert` places a value and shifts the rest up; `push` then bubble.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(10)
  v.push(30)
  v.insert(1, 20)
  if v.len() != 3 {
    ret 1
  }
  if *v.at(0).or_panic_with_message("expected a value") != 10 {
    ret 2
  }
  if *v.at(1).or_panic_with_message("expected a value") != 20 {
    ret 3
  }
  if *v.at(2).or_panic_with_message("expected a value") != 30 {
    ret 4
  }
  v.insert(0, 5)
  if *v.at(0).or_panic_with_message("expected a value") != 5 {
    ret 5
  }
  if *v.at(1).or_panic_with_message("expected a value") != 10 {
    ret 6
  }
  v.insert(4, 99)
  if *v.at(4).or_panic_with_message("expected a value") != 99 {
    ret 7
  }
  if v.len() != 5 {
    ret 8
  }
  mut s := Vec::<String>::new()
  s.push(format("a"))
  s.push(format("c"))
  s.insert(1, format("b"))
  if s.len() != 3 {
    ret 9
  }
  if s.at(1).or_panic_with_message("expected a value").as_str() != "b" {
    ret 10
  }
  if s.at(2).or_panic_with_message("expected a value").as_str() != "c" {
    ret 11
  }
  ret 0
}
