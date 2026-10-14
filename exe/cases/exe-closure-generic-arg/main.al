// A function type inferred for a generic parameter is an argument
// like any other: the body is checked at the call, so the parameter
// is the concrete function type where the body returns or calls it.

fn id<T>(x: T) -> T {
  ret x
}

fn apply<T>(f: T, x: i32) -> i32 {
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
