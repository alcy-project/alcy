fn main() -> i32 {
  ret 0
}

pub fn alcy_test_run(index: i64) -> bool {
  ret true
}

#[when(testing)] {
  #[test] fn works() {
    if 1 + 1 != 2 {
      panic("addition")
    }
  }
}
