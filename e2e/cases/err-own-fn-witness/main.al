// A witness that carries no function signature cannot satisfy an
// owning function parameter.
fn apply(f: fn(i32) -> i32, x: i32) -> i32 {
  ret f(x)
}

fn main() -> i32 {
  ret apply(5, 1)
}
