// A debug build checks the divisors the backend leaves undefined;
// `--release` keeps the backend's own behavior.
fn main() -> i32 {
  b := 0i32
  ret 5 / b
}
