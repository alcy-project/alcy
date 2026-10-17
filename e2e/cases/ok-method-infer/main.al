// A method's type parameters infer from the call's arguments the way a
// free function's do: a declaration pins what it can, a closure's own
// signature fills a return slot, and an owning `fn()` parameter binds
// its witness from the argument.
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
  if h.map_to((x: &i32) -> format("{}", *x)).as_str() != "5" { ret 1 }
  r := Runner { base: 10 }
  s := String::from("ab")
  if r.run([s] (x: i32) -> x + s.len() as i32, 1) != 13 { ret 2 }
  ret 0
}
