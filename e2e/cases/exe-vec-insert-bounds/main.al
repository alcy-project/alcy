// `Vec::insert` refuses an index past the end instead of appending: the
// length is the last place a value may be inserted at, so a larger index
// panics before the buffer is touched.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.insert(0, 2)
  println("{} {}", v[0], v[1])
  v.insert(3, 9)
  println("unreachable")
  ret 0
}
