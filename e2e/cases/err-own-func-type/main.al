// Owning function types parse but wait for their slice: the checker
// refuses them rather than letting one through as a borrowing view.
fn apply(f: fn(i32) -> i32, x: i32) -> i32 {
  ret f(x)
}

fn main() -> i32 {
  ret apply((a: i32) -> a, 1)
}
