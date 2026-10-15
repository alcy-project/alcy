// `String::reserve` makes room for more bytes up front.
fn main() -> i32 {
  mut s := String::new()
  s.reserve(64)
  if s.capacity() < 64 { ret 1 }
  s.push_str("hi")
  if s.as_str() != "hi" { ret 2 }
  s.reserve(1)
  if s.as_str() != "hi" { ret 3 }
  ret 0
}
