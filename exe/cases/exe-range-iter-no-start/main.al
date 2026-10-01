// A range with no start has no first value: naming a cursor for one
// fails rather than guessing a minimum.
fn main() -> i32 {
  mut n := 0
  for i in ..<5 {
    n = n + i
  }
  ret n
}
