// The wrapping forms evaluate at compile time without the overflow
// check: the program said the wrap is intended (ADR-0065).
const N: u32 = wrapping_mul(3 as u32, 4294967294 as u32)

fn main() -> i32 {
  if N != 4294967290 as u32 {
    ret 1
  }
  ret 0
}
