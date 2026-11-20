// `wrapping_mul` is the program saying the wrap is intended, so a
// debug build takes it without a check.
fn main() -> i32 {
  if wrapping_mul(3 as u32, 4294967294 as u32) == 4294967290 as u32 {
    ret 7
  }
  ret 1
}
