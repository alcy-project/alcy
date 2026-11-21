use io::print::print;

fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn prints() {
    print("hello from a test")
  }
}
