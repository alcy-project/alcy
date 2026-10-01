// A stride must be positive: zero has no meaning as a step, so
// asking for one fails rather than yielding one value forever.
fn main() -> i32 {
  mut n := 0
  for i in (0..<10).step_by(0) {
    n = n + i
  }
  ret n
}
