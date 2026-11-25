// A cast from a float to an integer saturates: a value below the
// destination takes its minimum, above it its maximum, and NaN becomes
// zero. The LLVM form was undefined and the wasm form trapped, so the
// same program answered differently or not at all.
fn main() -> i32 {
  z := 0.0f64
  nan := z / z
  inf := 1.0f64 / z
  ninf := -1.0f64 / z
  big := 1.0e30f64
  if nan as i32 != 0 {
    ret 1
  }
  if inf as i32 != 2147483647i32 {
    ret 2
  }
  if ninf as i32 != -2147483648i32 {
    ret 3
  }
  if big as i32 != 2147483647i32 {
    ret 4
  }
  if big as i64 != 9223372036854775807i64 {
    ret 5
  }
  if inf as i8 != 127i8 {
    ret 6
  }
  if ninf as i16 != -32768i16 {
    ret 7
  }
  if nan as u8 != 0u8 {
    ret 8
  }
  if (-1.5f64) as u32 != 0u32 {
    ret 9
  }
  if inf as u64 != 18446744073709551615u64 {
    ret 10
  }
  if (nan as f32) as i32 != 0 {
    ret 11
  }
  narrowed := big as f32
  if narrowed as i64 != 9223372036854775807i64 {
    ret 12
  }
  // A value in range still truncates toward zero.
  if 3.9f64 as i32 != 3 {
    ret 13
  }
  if (-3.9f64) as i32 != -3 {
    ret 14
  }
  // The narrow unsigned band: a word above 2^31 compared signed
  // misses the clamp and truncates to the wrong byte; bool saturates
  // at its one bit.
  if (3000000000.0f64) as u8 != 255u8 {
    ret 16
  }
  if (2147483648.0f64) as u16 != 65535u16 {
    ret 17
  }
  if 1.5f64 as bool != true {
    ret 18
  }
  if nan as bool != false {
    ret 19
  }
  if 7.0f64 as u8 != 7u8 {
    ret 15
  }
  // A value inside the unsigned range must not saturate: a 32-bit
  // signed convert cannot hold 3e9, and a 64-bit signed convert
  // cannot hold 1e19.
  if (3000000000.0f64) as u32 != 3000000000u32 {
    ret 20
  }
  if (10000000000.0e9) as u64 != 10000000000000000000u64 {
    ret 21
  }
  if (4294967295.0f64) as u32 != 4294967295u32 {
    ret 22
  }
  // A u64 source is unsigned: its sign bit adds 2^64 to the answer.
  if (18446744073709551615u64 as f64) != 18446744073709551615.0f64 {
    ret 23
  }
  // A float cast to bool saturates to zero or one, not the byte range.
  if (200.0f64 as bool) != true {
    ret 24
  }
  if ((-200.0f64) as bool) != false {
    ret 25
  }
  ret 0
}
