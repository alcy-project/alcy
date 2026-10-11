// The release passes fold constants and prune what they leave dead;
// the direct wasm backend has no middle end of its own, so this is its
// optimizer. The loop also gives the pass run something to fold.

fn main() -> i32 {
  const offset := 7 * 3
  mut total := 0
  mut i := 1
  while i <= 5 {
    total = total + i * i
    i = i + 1
  }
  println("sum {}", total + offset)
  ret 0
}
