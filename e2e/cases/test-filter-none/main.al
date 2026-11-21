fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn adds() {
    if 1 + 1 != 2 {
      panic("addition")
    }
  }
}
