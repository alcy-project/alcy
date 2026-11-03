fn main() -> i32 {
  mut n := 5
  mut r := &n
  {
    mut m := 9
    pair := (&m, 1)
    r = pair.0
  }
  ret *r
}
