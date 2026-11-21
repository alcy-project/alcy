fn main() -> i32 {
  ret 0
}

fn helper() {
  ret
}

#[when(testing)] {
  #[test] fn helper() {
    ret
  }
}
