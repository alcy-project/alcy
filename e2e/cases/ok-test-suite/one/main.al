fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn one_passes() {
    if 1 != 1 {
      panic("impossible")
    }
  }
}
