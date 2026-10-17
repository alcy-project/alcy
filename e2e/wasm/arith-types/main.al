// The released arithmetic of every integer type at its boundaries: the
// signed and unsigned forms of divide, shift, and compare pick the opcode
// the type asks for, and a narrow result carries the value the type
// names. The cases here run through the same IR the differential tables
// covered; this pins the shapes that matter most under wasm.
fn main() -> i32 {
  if (-128i8 / 2i8) != -64i8 { ret 1 }
  if (255u8 / 2u8) != 127u8 { ret 2 }
  if (255u8 >> 1) != 127u8 { ret 3 }
  if (-128i8 >> 1) != -64i8 { ret 4 }
  if !(128u8 > 127u8) { ret 5 }
  if !(-1i8 < 0i8) { ret 6 }
  if (-32768i16 / -1i16) != -32768i16 { ret 7 }
  if (65535u16 / 2u16) != 32767u16 { ret 8 }
  if (65535u16 >> 1) != 32767u16 { ret 9 }
  if (-32768i16 >> 1) != -16384i16 { ret 10 }
  if (-2147483648i32 / -1i32) != -2147483648i32 { ret 11 }
  if (4294967295u32 / 2u32) != 2147483647u32 { ret 12 }
  if (4294967295u32 >> 1) != 2147483647u32 { ret 13 }
  if !(4294967295u32 > 0u32) { ret 14 }
  if (-9223372036854775808i64 / -1i64) != -9223372036854775808i64 { ret 15 }
  if (18446744073709551615u64 / 2u64) != 9223372036854775807u64 { ret 16 }
  if (18446744073709551615u64 >> 1) != 9223372036854775807u64 { ret 17 }
  if !(18446744073709551615u64 > 0u64) { ret 18 }
  if !(-2147483648i32 < 0i32) { ret 19 }
  if (-2147483648i32 >> 1) != -1073741824i32 { ret 20 }
  if (-9223372036854775808i64 >> 1) != -4611686018427387904i64 { ret 21 }
  ret 0
}
