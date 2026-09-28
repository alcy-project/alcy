// `at` hands out a reference into the vector's own buffer. Holding it
// across a `push` that reallocates leaves the result pointing at the
// block `push` released, so the checker has to report the conflict
// rather than leave it to run time.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(11i32)
  held := v.at(0)
  v.push(22i32)
  r := held.unwrap()
  ret *r - 11
}
