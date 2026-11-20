// A negative exponent has no integer answer, and the run panics the
// way a zero divisor does instead of reaching a backend's answer.
fn main() -> i32 {
  mut exponent := -2
  ret 3 ** exponent
}
