// `Vec::swap_remove` refuses an index past the end before it reads the
// last element, so an empty vector cannot subtract below zero.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  println("{}", v.swap_remove(0))
  v.swap_remove(1)
  println("unreachable")
  ret 0
}
