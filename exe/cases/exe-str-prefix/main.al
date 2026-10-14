fn has(s: str) -> bool { ret s.starts_with("he") }
fn main() -> i32 {
  if !has("hello") { ret 1 }
  if has("world") { ret 2 }
  s := format("hello")
  if !s.starts_with("he") { ret 3 }
  if s.starts_with("lo") { ret 4 }
  if !s.ends_with("lo") { ret 5 }
  if s.ends_with("he") { ret 6 }
  if !s.starts_with("") { ret 7 }
  if !s.ends_with("") { ret 8 }
  if s.starts_with("hello world") { ret 9 }
  if s.ends_with("world") { ret 10 }
  if !"abc".starts_with("ab") { ret 11 }
  if !"abc".ends_with("bc") { ret 12 }
  ret 0
}
