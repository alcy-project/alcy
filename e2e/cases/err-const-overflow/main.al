// A constant expression is evaluated by the compiler, and an overflow
// there is refused rather than read as wrapping (ADR-0062).
const N: i32 = 2147483647 + 1

fn main() -> i32 {
  ret N
}
