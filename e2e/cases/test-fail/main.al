fn main() -> i32 {
  ret 0
}

test {
  test passing() {
    ret
  }

  test failing() {
    panic("boom")
  }
}
