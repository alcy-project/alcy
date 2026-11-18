// `is_utf8` answers whether a byte sequence is well-formed UTF-8:
// no overlong form, no surrogate, nothing above the last scalar, and
// no truncated sequence.

fn main() -> i32 {
  if !is_utf8(&[0x61u8, 0x62u8, 0x63u8]) {
    ret 1
  }
  if !is_utf8(&[0xc3u8, 0xa9u8]) {
    ret 2
  }
  empty := &[0u8; 0]
  if !is_utf8(empty) {
    ret 3
  }
  if valid(&[0xe1u8, 0x80u8]) {
    ret 4
  }
  if valid(&[0xf0u8, 0x80u8, 0x80u8]) {
    ret 5
  }
  if valid(&[0xedu8, 0xa0u8, 0x80u8]) {
    ret 6
  }
  if valid(&[0xf4u8, 0x90u8, 0x80u8, 0x80u8]) {
    ret 7
  }
  if valid(&[0x80u8]) {
    ret 8
  }
  ret 0
}
