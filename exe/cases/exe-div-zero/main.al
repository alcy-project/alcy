// A zero divisor panics rather than reaching a backend's undefined
// division by zero.

fn main() -> i32 {
  mut divisor := 0
  ret 10 / divisor
}
