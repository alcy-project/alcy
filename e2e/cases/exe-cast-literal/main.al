// An unsuffixed integer literal keeps its default type through a cast
// that cannot hold it, so the cast truncates: `300 as u8` is 44. A
// literal that only the target holds still takes the target, so a
// magnitude past `i32` is spellable.
fn main() -> i32 {
  if (300 as u8) != 44u8 {
    ret 1
  }
  if (0 - 300) as u8 != 212u8 {
    ret 2
  }
  if 9223372036854775808 as u64 != 9223372036854775808u64 {
    ret 3
  }
  if 18446744073709551615 as u64 != 18446744073709551615u64 {
    ret 4
  }
  ret 0
}
