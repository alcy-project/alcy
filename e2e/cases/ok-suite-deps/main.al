fn main() -> i32 {
  got := Option::<i32>::Some(41i32)
  if got.unwrap() != 41 {
    ret 1
  }
  mut v := Vec::<i32>::new()
  v.push(1i32)
  if v.len() != 1 {
    ret 2
  }
  print(format("n={}", (7i32,)).as_str())
  ret 0
}
