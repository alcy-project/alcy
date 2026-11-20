// A constant comparison folds at the operand's type, not at the bool its
// destination carries. The release folder once ranked `-1i8 < 0i8`, and
// every other signed pair, by the raw words: an all-constant branch in a
// release build took the wrong arm while the debug build was right.
fn main() -> i32 {
  // Signed pairs that straddle zero, at every width.
  if !(-1i8 < 0i8) {
    ret 1
  }
  if !(-128i8 < 127i8) {
    ret 2
  }
  if !(-1i16 < 0i16) {
    ret 3
  }
  if !(-32768i16 < 32767i16) {
    ret 4
  }
  if !(-1i32 < 0i32) {
    ret 5
  }
  if !(-2147483648i32 < 2147483647i32) {
    ret 6
  }
  if !(-1i64 < 0i64) {
    ret 7
  }
  if !(-9223372036854775808i64 < 9223372036854775807i64) {
    ret 8
  }
  // An unsigned type keeps its reading.
  if 255u8 < 0u8 {
    ret 9
  }
  if 18446744073709551615u64 < 0u64 {
    ret 10
  }
  // The other ordering folds at the same place.
  if !(0i8 > -1i8) {
    ret 11
  }
  if !(-1i8 <= 0i8) {
    ret 12
  }
  if !(0i8 >= -1i8) {
    ret 13
  }
  if -1i8 >= 0i8 {
    ret 14
  }
  if 1u8 <= 0u8 {
    ret 15
  }
  // Propagation carries the same values through bindings.
  a := -1i8
  b := 0i8
  if !(a < b) {
    ret 16
  }
  if a > b {
    ret 17
  }
  ret 0
}
