fn inc(x: i32) -> i32 {
  ret x + 1
}

fn apply(f: (i32) -> i32, x: i32) -> i32 {
  ret f(x)
}

fn main() -> i32 {
  // A named function coerces to a closure value: the same call
  // through a literal and through a name must agree.
  if apply(inc, 41) != apply((a: i32) -> a + 1, 41) { ret 1 }
  h := inc
  if h(41) != 42 { ret 2 }
  ret 0
}
