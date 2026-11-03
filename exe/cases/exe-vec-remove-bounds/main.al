// `Vec::remove` refuses an index past the end before its shift loop:
// the loop adds one to the index, so an unchecked `usize::MAX` would
// wrap to zero and exchange the first element with the last.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  println("{}", v.remove(0))
  v.remove(18446744073709551615usize)
  println("unreachable")
  ret 0
}
