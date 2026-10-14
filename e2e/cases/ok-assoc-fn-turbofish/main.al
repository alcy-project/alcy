struct Pair { a: i32 }

impl Pair {
  fn id<T>(x: T) -> T {
    ret x
  }
}

fn main() -> i32 {
  ret Pair::id::<i32>(7) - 7
}
