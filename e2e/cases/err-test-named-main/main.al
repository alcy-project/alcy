fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn main() {
    ret
  }
}
