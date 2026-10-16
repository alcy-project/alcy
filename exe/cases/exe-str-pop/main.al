// `pop` takes the last byte and shortens the view.
fn main() -> i32 {
  mut s := format("ab")
  if s.pop().unwrap() != 98u8 {
    ret 1
  }
  if s.as_str() != "a" {
    ret 2
  }
  if s.pop().unwrap() != 97u8 {
    ret 3
  }
  if s.pop().is_some() {
    ret 4
  }
  if s.len() != 0 {
    ret 5
  }
  mut t := String::new()
  if t.pop().is_some() {
    ret 6
  }
  t.push(65u8)
  if t.pop().unwrap() != 65u8 {
    ret 7
  }
  ret 0
}
