// A generic impl carries a method's own type parameters: the call
// spells them, the declaration is still the check, and each instance
// gets its own symbol and call table.

spec Pick {
  fn pick<T>(self: &Self, x: T) -> T;
}

struct Wrap<V> {
  value: V,
}

impl<V> Pick for Wrap<V> {
  fn pick<U>(self: &Self, x: U) -> U {
    ret x
  }
}

fn main() -> i32 {
  w := Wrap::<i32> { value: 1 }
  if w.pick::<i32>(7) != 7 {
    ret 1
  }
  s := Wrap::<i32> { value: 2 }
  if s.pick::<String>(format("ok")).len() != 2 {
    ret 2
  }
  if w.pick::<i32>(w.value) != 1 {
    ret 3
  }
  ret 0
}
