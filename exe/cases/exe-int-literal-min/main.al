// A signed minimum fits as a negated literal even though its positive
// magnitude is one past the type.
fn main() -> i32 {
  a := -128i8
  if a != (0 - 127i8) - 1i8 { ret 1 }
  b := -2147483648i32
  if b != (0 - 2147483647i32) - 1i32 { ret 2 }
  c := -9223372036854775808i64
  if c != (0 - 9223372036854775807i64) - 1i64 { ret 3 }
  ret 0
}
