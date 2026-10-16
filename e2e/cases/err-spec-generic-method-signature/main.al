// The implementation is compared against the declaration, so a
// return type that does not answer the method's own parameter is
// refused where the impl is written.
spec Pick {
  fn pick<T>(self: &Self, x: T) -> T;
}

struct W {
  base: i32,
}

impl Pick for W {
  fn pick<U>(self: &Self, x: U) -> i32 {
    ret self.base
  }
}

fn main() -> i32 {
  w := W { base: 7 }
  ret w.pick::<i32>(5)
}
