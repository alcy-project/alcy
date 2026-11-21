fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn passing() {
    ret
  }

  #[test] fn failing() {
    panic("boom")
  }
}
