fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn two_passes() {
    if 1 != 1 {
      panic("impossible")
    }
  }
}
