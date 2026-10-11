// Every arithmetic result is defined: a shift count rides modulo the
// type's width, and a signed `MIN / -1` wraps instead of overflowing.

fn main() -> i32 {
  // Compile-time evaluation follows the same rules.
  const over := 1i32 << 33
  const wrap := -128i8 / -1i8
  if over != 2 {
    ret 1
  }
  if wrap != -128i8 {
    ret 2
  }

  // Overshifts mask, on both a literal and a variable count.
  if 1i32 << 32 != 1 {
    ret 3
  }
  if 1i32 << 33 != 2 {
    ret 4
  }
  mut count := 32
  if 1i32 << count != 1 {
    ret 5
  }
  if 255u8 >> 8 != 255u8 {
    ret 6
  }
  if -128i8 >> 8 != -128i8 {
    ret 7
  }
  if -1i8 >> 9 != -1i8 {
    ret 8
  }

  // `MIN / -1` and `MIN % -1` are values, not overflows.
  if -128i8 / -1i8 != -128i8 {
    ret 9
  }
  if -128i8 % -1i8 != 0 {
    ret 10
  }
  if 7i32 / 2 != 3 {
    ret 11
  }
  if 7i32 % 2 != 1 {
    ret 12
  }
  ret 0
}
