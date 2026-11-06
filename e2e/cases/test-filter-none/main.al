fn main() -> i32 {
  ret 0
}

test {
  test adds() {
    if 1 + 1 != 2 {
      panic("addition")
    }
  }
}
