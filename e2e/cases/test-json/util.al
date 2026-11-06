pub fn twice(value: i64) -> i64 {
  ret value * 2
}

pub fn assert_eq(left: i64, right: i64) {
  if left != right {
    panic("not equal")
  }
}

test {
  test doubles() {
    assert_eq(twice(3), 6)
  }
}
