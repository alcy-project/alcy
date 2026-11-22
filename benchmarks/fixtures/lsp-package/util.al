// A sibling the package reading resolves against: an edit to main
// invalidates the reading the two share.
pub fn sum_to(n: i32) -> i32 {
  mut total := 0
  mut i := 0
  while i <= n {
    total += i
    i += 1
  }
  ret total
}
