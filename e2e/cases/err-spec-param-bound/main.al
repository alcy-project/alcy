// A spec parameter states a bound on the specification itself, and an
// implementation may spell the bound out or leave it off: the
// declaration is what a call owes, and the call is where an argument
// becomes concrete enough to compare.
spec Checkable<T: PartialEq> {
  fn check(self: &Self, other: &T) -> bool;
}

struct P {
  base: i32,
}

struct Q {
  base: i32,
}

impl Checkable < Q > for P {
  fn check(self: &P, other: &Q) -> bool {
    ret self.base == other.base
  }
}

fn main() -> i32 {
  p := P { base: 1 }
  q := Q { base: 2 }
  if p.check(&q) {
    ret 1
  }
  ret 0
}
