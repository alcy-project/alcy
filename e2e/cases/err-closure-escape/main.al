fn make() -> (i32) -> i32 {
  t := 5
  ret [&t] (a: i32) -> a + t
}

fn main() -> i32 {
  f := make()
  ret f(1)
}
