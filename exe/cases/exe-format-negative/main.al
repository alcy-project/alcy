fn main() -> i32 {
  a := format("{}", (-1i32,))
  b := format("{}", (-42i32,))
  c := format("{} {} {}", (0i32, 7i32, -3i32))
  print(a.as_str())
  print(" ")
  print(b.as_str())
  print(" ")
  print(c.as_str())
  print("\n")
  ret 0
}
