// A block's parameters bind through its target type's arguments: the
// target is the only place an instantiation reads them from. A
// parameter the target never names has nowhere to bind, so the block
// states a block it cannot specialize and the declaration is the error.

spec Tagged<T> {
  fn tag(self: &Self, other: &T) -> i32;
}

struct P {
  base: i32,
}

impl<U> Tagged < U > for P {
  fn tag(self: &P, other: &U) -> i32 {
    ret self.base
  }
}

fn main() -> i32 {
  ret 0
}
