fn main() -> i32 {
  ret 0
}

test {
  test adds() {
    assert_eq(util::twice(2), 4)
  }

  fn assert_eq(left: i64, right: i64) {
    if left != right {
      panic("not equal")
    }
  }
}
