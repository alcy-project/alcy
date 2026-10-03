fn id<T>(x: T) -> T {
  ret x
}

fn main() -> i32 {
  f := id((a: i32) -> a)
  ret f(1)
}
