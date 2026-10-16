// Narrow arithmetic wraps inside the type: the value a compare reads is
// the one the language answers, whether the build runs the checked
// form or the release one. The i8 wraps go past 127 in the container,
// and the shift goes past the byte.
fn main() -> i32 {
  if -128i8 / -1i8 != -128i8 {
    ret 1
  }
  if -128i8 % -1i8 != 0i8 {
    ret 2
  }
  if 100i8 + 100i8 != -56i8 {
    ret 3
  }
  if 100i8 * 2i8 != -56i8 {
    ret 4
  }
  if 15i8 * 15i8 != -31i8 {
    ret 5
  }
  if 1i8 << 7 != -128i8 {
    ret 6
  }
  if -1i8 >> 3 != -1i8 {
    ret 7
  }
  if 255u8 >> 4 != 15u8 {
    ret 8
  }
  if -32768i16 / -1i16 != -32768i16 {
    ret 9
  }
  if 10000i16 * 10000i16 != -7936i16 {
    ret 10
  }
  // Variables, so the release passes cannot fold the runtime path away:
  // these are the opcodes this fixed.
  mut a: i8 := 100i8
  mut b: i8 := 100i8
  if a + b != -56i8 {
    ret 11
  }
  if a * b != 16i8 {
    ret 12
  }
  mut s: i8 := 1i8
  mut n: i8 := 7i8
  if s << n != -128i8 {
    ret 13
  }
  mut d: i8 := -128i8
  mut m: i8 := -1i8
  if d / m != -128i8 {
    ret 14
  }
  if d % m != 0i8 {
    ret 15
  }
  ret 0
}
