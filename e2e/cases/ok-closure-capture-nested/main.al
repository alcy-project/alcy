fn main() -> i32 {
  t := 5
  outer := [&t] (a: i32) -> {
    inner := [&t] (b: i32) -> b + t
    ret inner(a)
  }
  ret outer(1)
}
