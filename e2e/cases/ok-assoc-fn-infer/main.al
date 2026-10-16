// An associated function's call binds the member's own parameters from
// its arguments, whether or not the owner spells its arguments.

struct Pair {
  a: i32,
}

impl Pair {
  fn id<T>(x: T) -> T {
    ret x
  }
}

struct Holder<T> {
  v: T,
}

impl<T> Holder<T> {
  fn map_to<U>(x: U) -> U {
    ret x
  }

  fn get(self: &Self) -> T {
    ret self.v
  }
}

fn main() -> i32 {
  if Pair::id(7) != 7 {
    ret 1
  }
  if Holder::<i32>::map_to(7) != 7 {
    ret 2
  }
  // The owner's argument binds the owner, not the member: the member's
  // own parameter takes what the argument is.
  if Holder::<i32>::map_to(true) != true {
    ret 3
  }
  h := Holder::<i32> { v: 3 }
  if h.get() != 3 {
    ret 4
  }
  ret 0
}
