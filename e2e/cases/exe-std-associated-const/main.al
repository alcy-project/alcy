// Integer limits are associated constants defined by core, not built
// into the language.
fn main() -> i32 {
  if i32::MAX != 2147483647i32 {
    ret 1
  }
  if i64::MIN != (0i64 - 9223372036854775807i64) - 1i64 {
    ret 2
  }
  if u64::MAX != 18446744073709551615u64 {
    ret 3
  }
  if !check_width() {
    ret 4
  }
  ret 0
}

#[when(target.width == 64)]
fn check_width() -> bool {
  ret usize::MAX == 18446744073709551615usize && isize::MIN == -9223372036854775808isize
}

#[when(target.width == 32)]
fn check_width() -> bool {
  ret usize::MAX == 4294967295usize && isize::MIN == -2147483648isize
}
