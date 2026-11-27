fn main() -> i32 {
  got := Option::<i32>::Some(3i32)
  if got.or_panic("expected a value") != 3 {
    ret 1
  }
  ret 0
}
