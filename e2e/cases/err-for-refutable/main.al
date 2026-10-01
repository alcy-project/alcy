// The `for` pattern must match every item: a refutable pattern is
// an error naming the loop, not a filter. Write the `loop` out when
// some items should skip the body.
fn count_zeroes(n: i32) -> i32 {
  mut zeroes := 0
  for 0 in 0..<n {
    zeroes = zeroes + 1
  }
  ret zeroes
}

fn count_small(n: i32) -> i32 {
  mut small := 0
  for 1 | 2 in 0..<n {
    small = small + 1
  }
  ret small
}

fn main() -> i32 {
  ret count_zeroes(5) + count_small(5)
}
