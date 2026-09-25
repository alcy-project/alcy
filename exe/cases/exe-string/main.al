fn main() -> i32 {
  mut bad := 0
  mut s := String::new()
  if s.len() != 0 {
    bad = 1
  }
  s.push(104u8)
  s.push(105u8)
  if s.len() != 2 {
    bad = 2
  }
  if str_len(s.as_str()) != 2 {
    bad = 3
  }
  if str_byte(s.as_str(), 0) != 104u8 {
    bad = 4
  }
  t := format("hi {}!", (s.as_str(),))
  if t.len() != 6 {
    bad = 5
  }
  if str_byte(t.as_str(), 0) != 104u8 {
    bad = 6
  }
  if str_byte(t.as_str(), 5) != 33u8 {
    bad = 7
  }
  n := format("a={} b={}", (41i32, true))
  if n.len() != 11 {
    bad = 8
  }
  e := format("hi", ())
  if e.len() != 2 {
    bad = 9
  }
  ret bad
}
