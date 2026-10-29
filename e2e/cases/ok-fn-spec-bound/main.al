// The spelled bound is the sugar's own capability (ADR-0116): a
// parameter bounded by `Fn<(A, B), R>` takes a named function, a
// closure literal - owning or not - and the body calls through the
// bound.
fn apply<F: Fn<(i32, i32), i32>>(f: F, a: i32, b: i32) -> i32 {
  ret f(a, b) + f(b, a)
}

fn add(a: i32, b: i32) -> i32 {
  ret a + b
}

fn run<F: Fn<(), i32>>(f: F) -> i32 {
  ret f() + f()
}

fn main() -> i32 {
  if apply(add, 1, 2) != 6 {
    ret 1
  }
  if apply((x: i32, y: i32) -> x * y, 3, 4) != 24 {
    ret 2
  }
  s := String::from("ab")
  if apply([s](x: i32, y: i32) -> x + y + s.len_bytes() as i32, 1, 1) != 8 {
    ret 3
  }
  if run(() -> 5) != 10 {
    ret 4
  }
  ret 0
}
