// The declaration is the check: an impl method that declares no type
// parameters cannot implement one that declares some.
spec Pick {
  fn pick<T>(self: &Self, x: T) -> T;
}

struct W {
  base: i32,
}

impl Pick for W {
  fn pick(self: &Self, x: i32) -> i32 {
    ret x
  }
}

fn main() -> i32 {
  ret 0
}
