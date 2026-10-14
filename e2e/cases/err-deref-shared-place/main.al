// A shared reference is not a place to write.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  *v.first().unwrap() = 10
  ret 0
}
