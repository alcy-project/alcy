// A spec method's declared bound holds at the call, even when the impl
// spells its own type parameter without one: the declaration is the
// contract the caller sees, and it is checked where the call is written.
spec Checkable {
  fn check<T: PartialEq>(self: &Self, x: T) -> bool;
}

struct P {
  base: i32,
}

impl Checkable for P {
  fn check<U>(self: &P, x: U) -> bool {
    ret true
  }
}

struct Q {
  base: i32,
}

fn main() -> i32 {
  p := P { base: 1 }
  q := Q { base: 2 }
  if p.check::<Q>(q) {
    ret 1
  }
  ret 0
}
