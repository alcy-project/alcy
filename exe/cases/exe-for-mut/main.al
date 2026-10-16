// Mutating iteration over a vector.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  for x in v.iter_mut() {
    *x = *x * 2
  }
  if *v.at(0).unwrap() != 2 {
    ret 1
  }
  if *v.at(1).unwrap() != 4 {
    ret 2
  }
  ret 0
}
