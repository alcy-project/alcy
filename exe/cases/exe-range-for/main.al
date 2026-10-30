// Counting loops over the two range spellings: `..<` half-open, `..=`
// inclusive.
fn main() -> i32 {
  mut sum := 0
  for i in 0..<4 {
    sum = sum + i
  }
  if sum != 6 {
    ret 1
  }
  mut closed := 0
  for i in 1..=4 {
    closed = closed + i
  }
  if closed != 10 {
    ret 2
  }
  ret 0
}
