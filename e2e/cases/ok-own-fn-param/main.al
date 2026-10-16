// A `fn(...)` parameter desugars to a synthetic generic parameter whose
// witness is the argument: a closure owns its environment, a named
// function or a view carries its function type, and the callee may
// call the value more than once.
fn apply(f: fn (i32) -> i32, x: i32) -> i32 {
  ret f(x) + f(x)
}

fn inc(x: i32) -> i32 {
  ret x + 1
}

fn main() -> i32 {
  s := String::from("ab")
  if apply([s](x: i32) -> x + s.len() as i32, 1) != 6 {
    ret 1
  }
  if apply(inc, 20) != 42 {
    ret 2
  }
  t := 10
  g: (i32) -> i32 := [&t](a: i32) -> a + t
  if apply(g, 1) != 22 {
    ret 3
  }
  ret 0
}
