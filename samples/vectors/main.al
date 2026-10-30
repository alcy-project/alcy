// A growable `Vec`, built up a push at a time and read back by index.
// `at` returns an `Option`, so a read is checked rather than assumed.
fn main() -> i32 {
  mut squares := Vec::<i32>::new()
  mut n := 1i32
  while n <= 6 {
    squares.push(n * n)
    n = n + 1
  }
  println("len = {} capacity = {}", squares.len(), squares.capacity())

  mut sum := 0i32
  mut i := 0 as usize
  while i < squares.len() {
    if Option::Some(v) := squares.at(i) {
      sum = sum + *v
    }
    i = i + 1
  }
  println("sum = {}", sum)

  if Option::Some(last) := squares.pop() {
    println("popped {}", last)
  }
  println("len = {}", squares.len())
  ret 0
}
