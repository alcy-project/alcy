fn main() -> i32 {
  ret 0
}

test {
  test adds() {
    util::assert_eq(util::twice(2), 4)
  }
}
