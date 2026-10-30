// `is_char_boundary` answers for a position a scalar may start at: the
// ends always qualify, a position inside a scalar does not.
fn main() -> i32 {
  s := "éx" // two bytes, then one
  if !s.is_char_boundary(0) {
    ret 1
  }
  if s.is_char_boundary(1) {
    ret 2
  }
  if !s.is_char_boundary(2) {
    ret 3
  }
  if !s.is_char_boundary(3) {
    ret 4
  }
  // Past the end is a boundary too.
  if !s.is_char_boundary(9) {
    ret 5
  }
  if !"".is_char_boundary(0) {
    ret 6
  }
  // A four-byte scalar: only its first and last bytes qualify.
  e := "😀"
  if !e.is_char_boundary(0) {
    ret 7
  }
  if e.is_char_boundary(1) || e.is_char_boundary(2) || e.is_char_boundary(3) {
    ret 8
  }
  if !e.is_char_boundary(4) {
    ret 9
  }
  ret 0
}
