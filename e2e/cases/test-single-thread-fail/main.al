fn main() -> i32 {
  ret 0
}

test {
  test fails() {
    panic("boom")
  }

  test later() {
    ret
  }
}
