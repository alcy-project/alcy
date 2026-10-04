struct S {}

impl S {
  // Named `main`, but associated: not the program entry.
  fn main() -> i32 { ret 1 }
}

fn main() -> i32 { ret 0 }
