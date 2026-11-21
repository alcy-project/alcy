fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn twice() {
    ret
  }

  #[test] fn twice() {
    ret
  }
}
