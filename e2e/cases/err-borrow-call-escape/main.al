fn id(x: &i32) -> &i32 {
  ret x
}

fn main() -> i32 {
  mut n := 5
  mut r := &n
  {
    mut m := 9
    r = id(&m)
  }
  ret *r
}
