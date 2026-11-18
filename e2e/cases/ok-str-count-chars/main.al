// `count_chars` counts scalar values. A well-formed `str` can only be
// well-formed, so the count is exact — nothing else is possible.
fn main() -> i32 {
  if "abc".count_chars() != 3 {
    ret 1
  }
  if "é".count_chars() != 1 {
    ret 2
  }
  if "日本語".count_chars() != 3 {
    ret 3
  }
  if is_utf8(&[0xe1u8, 0x80u8]) {
    ret 4
  }
  if is_utf8(&[0xe6u8, 0x97u8, 0xa5u8]) {} else {
    ret 5
  }
  if "".count_chars() != 0 {
    ret 6
  }
  ret 0
}
