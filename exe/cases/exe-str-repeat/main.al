// `repeat` copies the bytes `n` times; a zero count is empty.
fn main() -> i32 {
  s1 := format("ab")
  a := s1.repeat(3)
  if a.as_str() != "ababab" { ret 1 }
  s2 := format("x")
  b := s2.repeat(0)
  if b.len() != 0 { ret 2 }
  s3 := format("")
  c := s3.repeat(5)
  if c.len() != 0 { ret 3 }
  s4 := format("-")
  d := s4.repeat(4)
  if d.as_str() != "----" { ret 4 }
  ret 0
}
