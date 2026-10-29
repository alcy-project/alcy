// A value that cannot be called does not satisfy the spelled bound.
fn apply<F: Fn<(i32,), i32>>(f: F, x: i32) -> i32 {
  ret f(x)
}

fn main() -> i32 {
  ret apply(1, 2)
}
