// Iterate a vector by value, consuming it.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  mut sum := 0
  for x in v {
    sum = sum + x
  }
  if sum != 3 {
    ret 1
  }
  ret 0
}
