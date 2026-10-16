// ASCII case folding allocates a new string; other bytes copy. The
// source is a named local so its buffer has an end site.
fn main() -> i32 {
  s1 := format("MiXeD")
  a := s1.to_ascii_lowercase()
  if a.as_str() != "mixed" {
    ret 1
  }
  s2 := format("MiXeD")
  b := s2.to_ascii_uppercase()
  if b.as_str() != "MIXED" {
    ret 2
  }
  s3 := format("abc123")
  c := s3.to_ascii_uppercase()
  if c.as_str() != "ABC123" {
    ret 3
  }
  s4 := format("")
  d := s4.to_ascii_lowercase()
  if d.len() != 0 {
    ret 4
  }
  s5 := format("already")
  e := s5.to_ascii_lowercase()
  if e.as_str() != "already" {
    ret 5
  }
  s6 := format("-- x")
  f := s6.to_ascii_uppercase()
  if f.as_str() != "-- X" {
    ret 6
  }
  ret 0
}
