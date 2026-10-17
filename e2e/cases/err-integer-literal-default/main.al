// An unsuffixed integer literal defaults to i32, and the spec checks
// the magnitude there: without the check the value truncated silently,
// so 4294967296 became 0.
fn main() -> i32 {
  x := 4294967296
  ret 0
}
