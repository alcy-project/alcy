// A spec method may declare type parameters, and a concrete impl
// names its own: the declaration is the check. A call may spell them
// or let the argument infer them.
spec Pick {
  fn pick<T>(self: &Self, x: T) -> T;
}

struct W {
  base: i32,
}

impl Pick for W {
  fn pick<U>(self: &Self, x: U) -> U {
    ret x
  }
}

fn main() -> i32 {
  w := W { base: 1 }
  if w.pick::<i32>(2) != 2 {
    ret 1
  }
  if w.pick(3) != 3 {
    ret 2
  }
  ret 0
}
