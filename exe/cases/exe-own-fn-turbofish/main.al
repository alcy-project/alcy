// A turbofish spells the parameters the declaration writes; a
// synthetic `fn(...)` slot takes its witness from the argument.
fn tagged<T>(tag: T, f: fn (i32) -> i32, x: i32) -> i32 {
  ret f(x) + tag
}

fn inc(x: i32) -> i32 {
  ret x + 1
}

struct Box2<T> {
  v: T,
}

impl<T> Box2<T> {
  fn pick<U>(self: &Self, a: U, f: fn (i32) -> i32) -> U {
    ret a
  }
}

fn main() -> i32 {
  if tagged::<i32>(7, inc, 1) != 9 {
    ret 1
  }
  b := Box2::<i32> { v: 1 }
  if b.pick::<i32>(5, inc) != 5 {
    ret 2
  }
  ret 0
}
