// A generic impl cannot carry a method's own type parameters yet: the
// impl's parameters would have to reach the method through the
// declaration.
spec Pick {
  fn pick<T>(self: &Self, x: T) -> T;
}

impl<T> Pick for Vec<T> {
  fn pick<U>(self: &Self, x: U) -> U {
    ret x
  }
}

fn main() -> i32 {
  ret 0
}
