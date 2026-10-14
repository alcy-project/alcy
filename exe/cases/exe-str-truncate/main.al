// `truncate` keeps a prefix and ignores a longer length.
fn main() -> i32 {
  mut s := format("hello")
  s.truncate(3)
  if s.as_str() != "hel" { ret 1 }
  s.truncate(10)
  if s.as_str() != "hel" { ret 2 }
  s.truncate(0)
  if s.len() != 0 { ret 3 }
  mut t := String::new()
  t.push_str("abc")
  t.truncate(1)
  if t.as_str() != "a" { ret 4 }
  ret 0
}
