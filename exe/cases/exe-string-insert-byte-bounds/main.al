// A byte insert past the length panics before the run past the
// position moves, and the string keeps the bytes it held.
fn main() -> i32 {
  mut s := format("ab")
  s.insert_byte(5, 0x58u8)
  if s.as_str() != "ab" {
    ret 1
  }
  ret 0
}
