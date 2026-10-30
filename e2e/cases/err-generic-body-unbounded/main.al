// A call on an unbounded type parameter is refused at the definition
// (ADR-0080): the body may only use what its bounds declare, and this
// one declares nothing.
struct Counter {
  n: i32,
}

impl Counter {
  fn bump(self: &Self) -> i32 {
    ret self.n
  }
}

fn bad<T>(x: T) -> i32 {
  ret x.bump()
}

fn main() -> i32 {
  ret 0
}
