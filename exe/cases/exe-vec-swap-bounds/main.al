// `Vec::swap` checks both indices, so one past the end panics before
// the exchange rather than reaching outside the block the buffer
// owns; an in-bounds call still crosses the two values, and the same
// index twice leaves both alone.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.swap(0, 1)
  println("{} {}", v[0], v[1])
  v.swap(1, 1)
  println("{} {}", v[0], v[1])
  v.swap(0, 1000000usize)
  println("unreachable")
  ret 0
}
