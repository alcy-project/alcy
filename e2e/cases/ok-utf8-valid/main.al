// `utf8::valid` answers whether a byte sequence is well-formed UTF-8:
// no overlong form, no surrogate, nothing above the last scalar, and
// no truncated sequence (ADR-0091).
fn main() -> i32 {
  if !valid("abc") {
    ret 1
  }
  if !valid("é") {
    ret 2
  }
  if !valid("") {
    ret 3
  }
  truncated := String::from_slice(&[0xe1u8, 0x80u8])
  if valid(truncated.as_str()) {
    ret 4
  }
  overlong := String::from_slice(&[0xf0u8, 0x80u8, 0x80u8])
  if valid(overlong.as_str()) {
    ret 5
  }
  surrogate := String::from_slice(&[0xedu8, 0xa0u8, 0x80u8])
  if valid(surrogate.as_str()) {
    ret 6
  }
  past_last := String::from_slice(&[0xf4u8, 0x90u8, 0x80u8, 0x80u8])
  if valid(past_last.as_str()) {
    ret 7
  }
  stray := String::from_slice(&[0x80u8])
  if valid(stray.as_str()) {
    ret 8
  }
  ret 0
}
