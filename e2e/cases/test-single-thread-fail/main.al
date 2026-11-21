fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn fails() {
    panic("boom")
  }

  #[test] fn later() {
    ret
  }
}
