// A string inserts a byte in the middle, at the front, and at the end.
fn main() -> i32 {
  mut s := format("ab")
  s.insert_byte(1, 0x58u8)
  if s.as_str() != "aXb" {
    ret 1
  }
  s.insert_byte(0, 0x59u8)
  if s.as_str() != "YaXb" {
    ret 2
  }
  n := s.len()
  s.insert_byte(n, 0x5au8)
  if s.as_str() != "YaXbZ" {
    ret 3
  }
  ret 0
}
