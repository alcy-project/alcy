// The specification's parameter carries a bound, and the argument the
// implementation names satisfies it, so the call goes through.
spec Named {
  fn name(self: &Self) -> i32;
}

spec Tagged<T: Named> {
  fn tag(self: &Self, other: &T) -> i32;
}

struct P {
  base: i32,
}

struct Q {
  base: i32,
}

impl Named for Q {
  fn name(self: &Q) -> i32 {
    ret self.base
  }
}

impl Tagged < Q > for P {
  fn tag(self: &P, other: &Q) -> i32 {
    ret self.base + other.name()
  }
}

fn main() -> i32 {
  p := P { base: 1 }
  q := Q { base: 2 }
  ret p.tag(&q) - 3
}
