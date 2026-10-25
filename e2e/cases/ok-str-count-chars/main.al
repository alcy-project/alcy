// `count_chars` counts scalar values and never fails: an ill-formed
// subsequence counts as one replacement unit (ADR-0091).
fn main() -> i32 {
  if "abc".count_chars() != 3 {
    ret 1
  }
  if "é".count_chars() != 1 {
    ret 2
  }
  // A truncated three-byte sequence is one unit.
  if String::from_slice(&[0xe1u8, 0x80u8]).as_str().count_chars() != 1 {
    ret 3
  }
  // An `0xf0` lead rejects `0x80` after it, so nothing is consumed:
  // three invalid bytes, three units.
  bad := String::from_slice(&[0xf0u8, 0x80u8, 0x80u8])
  if bad.as_str().count_chars() != 3 {
    ret 4
  }
  // A surrogate lead is one unit, and the trail bytes that do not fit
  // are invalid leads of their own: three units.
  surrogate := String::from_slice(&[0xedu8, 0xa0u8, 0x80u8])
  if surrogate.as_str().count_chars() != 3 {
    ret 5
  }
  if "".count_chars() != 0 {
    ret 6
  }
  ret 0
}
