// Discarding ends the value exactly once.
struct T {
  n: i32,
}

impl T {
  fn drop(mut self: T) {
    print("{}", self.n)
  }
}

fn main() -> i32 {
  a := T { n: 1 }
  _ := a
  ret 0
}
