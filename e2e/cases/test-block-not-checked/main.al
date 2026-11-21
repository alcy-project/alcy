fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn calls_nothing_that_exists() {
    missing()
  }
}
