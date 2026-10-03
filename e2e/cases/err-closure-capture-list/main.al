fn main() -> i32 {
  t := 5
  f := [t] (a: i32) -> a + t
  ret f(1)
}
