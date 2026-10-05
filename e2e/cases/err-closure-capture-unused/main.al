fn main() -> i32 {
  t := 5
  f := [&t] (a: i32) -> a
  ret f(1)
}
