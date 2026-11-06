fn main() -> i32 {
  ret 0
}

test {
  test calls_nothing_that_exists() {
    missing()
  }
}
