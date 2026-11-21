pub fn twice(value: i64) -> i64 {
  ret value * 2
}

#[when(testing)] {
  #[test] fn doubles() {
    if twice(3) != 6 {
      panic("twice")
    }
  }
}
