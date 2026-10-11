// The same source `+` wraps in a release build: the overflow policy is
// the build's, not the module's (ADR-0062).
fn main() -> i32 {
  mut a := 2147483647i32
  a = a + 1
  if a == -2147483648i32 {
    ret 7
  }
  ret 1
}
