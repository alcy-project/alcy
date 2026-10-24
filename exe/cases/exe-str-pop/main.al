// `pop` takes the last byte and shortens the view.
fn main() -> i32 {
  mut s := format("ab")
  if s.pop_byte().or_panic_with_message("expected a value") != 98u8 {
    ret 1
  }
  if s.as_str() != "a" {
    ret 2
  }
  if s.pop_byte().or_panic_with_message("expected a value") != 97u8 {
    ret 3
  }
  if s.pop_byte().is_some() {
    ret 4
  }
  if s.len_bytes() != 0 {
    ret 5
  }
  mut t := String::new()
  if t.pop_byte().is_some() {
    ret 6
  }
  t.push(65u8)
  if t.pop_byte().or_panic_with_message("expected a value") != 65u8 {
    ret 7
  }
  ret 0
}
