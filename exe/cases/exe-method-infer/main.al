// A method's type parameters infer from the call's arguments the way a
// free function's do, and a method's `fn()` parameter takes an owning
// closure the way a free function's does. Both calls run here; the
// witnesses hold only Copy values.
struct Holder<T> { value: T }

impl<T> Holder<T> {
  fn map_to<U>(self: &Self, f: (&T) -> U) -> U {
    ret f(&self.value)
  }
}

struct Runner { base: i32 }

impl Runner {
  fn run(self: &Self, f: fn(i32) -> i32, x: i32) -> i32 {
    ret f(x) + self.base
  }
}

fn main() -> i32 {
  h := Holder::<i32> { value: 5 }
  if h.map_to((x: &i32) -> *x * 2) != 10 { ret 1 }
  r := Runner { base: 10 }
  n := 2
  if r.run([n] (x: i32) -> x + n, 1) != 13 { ret 2 }
  ret 0
}
