fn main() -> i32 {
  mut n := 5
  mut r := &n
  {
    mut m := 9
    p := &m
    r = p
  }
  ret *r
}
