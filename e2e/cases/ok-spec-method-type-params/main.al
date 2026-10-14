// A spec method may declare type parameters, and a concrete impl
// names its own: the declaration is the check.
spec Pick {
  fn pick<T>(self: &Self, x: T) -> T;
}

struct W { base: i32 }

impl Pick for W {
  fn pick<U>(self: &Self, x: U) -> U {
    ret x
  }
}

fn main() -> i32 {
  w := W { base: 1 }
  ret w.pick::<i32>(2) - 2
}
