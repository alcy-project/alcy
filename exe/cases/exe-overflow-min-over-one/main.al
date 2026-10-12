// The one signed division that overflows: a checked build aborts on
// MIN / -1 (ADR-0068); `--release` wraps it (ADR-0066).
fn main() -> i32 {
  a := -128i8
  b := -1i8
  ret (a / b) as i32
}
