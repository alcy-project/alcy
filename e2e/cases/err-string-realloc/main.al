// `as_str` views the string's own buffer. Holding it across a `push`
// that reallocates leaves the view pointing at the block `push`
// released, so the checker has to report the conflict rather than
// leave it to run time.
fn main() -> i32 {
  mut s := String::new()
  s.push_char('h')
  r := s.as_str()
  s.push_char('i')
  print("{}", r)
  ret 0
}
