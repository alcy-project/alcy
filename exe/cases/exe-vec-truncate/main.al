// `truncate` ends the elements past the new length and keeps the
// rest, including for elements with a destructor.

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  v.push(2i32)
  v.push(3i32)
  v.truncate(1)
  if v.len() != 1 {
    ret 1
  }
  if *v.at(0).or_panic_with_message("expected a value") != 1 {
    ret 2
  }
  v.truncate(5)
  if v.len() != 1 {
    ret 3
  }
  mut s := Vec::<String>::new()
  s.push(format("a"))
  s.push(format("b"))
  s.truncate(0)
  if s.len() != 0 {
    ret 4
  }
  s.push(format("c"))
  if s.len() != 1 {
    ret 5
  }
  ret 0
}
