fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  if v["x"] != 1 {
    ret 1
  }
  ret 0
}
