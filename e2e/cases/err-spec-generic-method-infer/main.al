// A method that declares its own type parameters is instantiated by
// the call's explicit arguments; nothing binds them otherwise.
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
  w := W { base: 7 }
  ret w.pick(5)
}
