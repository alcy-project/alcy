// `pop` takes the last byte and shortens the view.
fn main() -> i32 {
  mut s := Vec::<u8>::new()
  s.push(97u8)
  s.push(98u8)
  if s.pop().or_panic_with_message("expected a value") != 98u8 {
    ret 1
  }
  if s.len() != 1 {
    ret 2
  }
  if s.pop().or_panic_with_message("expected a value") != 97u8 {
    ret 3
  }
  if s.pop().is_some() {
    ret 4
  }
  if s.len() != 0 {
    ret 5
  }
  mut t := Vec::<u8>::new()
  if t.pop().is_some() {
    ret 6
  }
  t.push(65u8)
  if t.pop().or_panic_with_message("expected a value") != 65u8 {
    ret 7
  }
  ret 0
}
