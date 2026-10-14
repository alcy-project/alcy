fn id<T>(x: T) -> T {
  ret x
}

fn main() -> i32 {
  f := id((a: i32) -> a + 1)
  ret f(1) - 2
}
