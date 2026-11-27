// A string inserts a single-byte run in the middle, at the front,
// and at the end.
fn main() -> i32 {
  mut s := format("ab")
  s.insert_str(1, "X").or_panic("offset 1 is a boundary")
  if s.as_str() != "aXb" {
    ret 1
  }
  s.insert_str(0, "Y").or_panic("offset 0 is a boundary")
  if s.as_str() != "YaXb" {
    ret 2
  }
  n := s.len_bytes()
  s.insert_str(n, "Z").or_panic("the end of the text is a boundary")
  if s.as_str() != "YaXbZ" {
    ret 3
  }
  ret 0
}
