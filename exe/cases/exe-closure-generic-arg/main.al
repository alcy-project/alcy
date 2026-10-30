// A generic parameter that is called states the callable bound: the
// body answers through `Fn` (ADR-0080), and a closure argument
// instantiates it like any other.

fn id<T>(x: T) -> T {
  ret x
}

fn apply<F: Fn<(i32,), i32>>(f: F, x: i32) -> i32 {
  ret f(x)
}

fn main() -> i32 {
  f := id((a: i32) -> a + 1)
  if f(1) != 2 {
    ret 1
  }
  if apply((a: i32) -> a * 2, 21) != 42 {
    ret 2
  }
  g := id((a: i64) -> a + 2)
  if g(1i64) != 3i64 {
    ret 3
  }
  ret 0
}
