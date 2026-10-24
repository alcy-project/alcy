fn main() -> i32 {
  got := Option::<i32>::Some(41i32)
  if got.or_panic_with_message("expected a value") != 41 {
    ret 1
  }
  mut v := Vec::<i32>::new()
  v.push(1i32)
  if v.len() != 1 {
    ret 2
  }
  print("n={}", 7i32)
  ret 0
}
