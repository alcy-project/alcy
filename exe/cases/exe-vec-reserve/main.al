// `reserve` grows the buffer without adding elements.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.reserve(10)
  if v.capacity() < 10 {
    ret 1
  }
  if v.len() != 0 {
    ret 2
  }
  v.push(1)
  if *v.at(0).unwrap() != 1 {
    ret 3
  }
  if v.capacity() < 10 {
    ret 4
  }
  v.reserve(100)
  if v.capacity() < 100 {
    ret 5
  }
  if v.len() != 1 {
    ret 6
  }
  ret 0
}
