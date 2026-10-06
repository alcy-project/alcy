fn main() -> i32 {
  mut t := 5
  u := 1
  mut v := 2
  f := [t, &u, &mut v] (a: i32) -> {
    v = v + a
    a + t + u
  }
  ret f(1) + v
}
