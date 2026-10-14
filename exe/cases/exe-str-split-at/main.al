// `split_at` answers the two views around a byte offset.
fn main() -> i32 {
  (a, b) := "hello".split_at(2)
  if a != "he" { ret 1 }
  if b != "llo" { ret 2 }
  (c, d) := "hello".split_at(0)
  if c != "" { ret 3 }
  if d != "hello" { ret 4 }
  (e, f) := "hello".split_at(5)
  if e != "hello" { ret 5 }
  if f != "" { ret 6 }
  s := format("abc")
  (g, h) := s.split_at(1)
  if g != "a" { ret 7 }
  if h != "bc" { ret 8 }
  ret 0
}
