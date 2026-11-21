fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn adds() {
    util::assert_eq(util::twice(2), 4)
  }
}
