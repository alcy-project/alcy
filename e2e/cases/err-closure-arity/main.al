fn apply(f: (i32) -> i32, x: i32) -> i32 {
  ret f(x)
}

fn main() -> i32 {
  ret apply((a: i32, b: i32) -> a, 1)
}
