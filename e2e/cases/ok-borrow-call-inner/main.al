fn id(x: &i32) -> &i32 {
  ret x
}

fn main() -> i32 {
  mut n := 5
  {
    r := id(&n)
    if *r != 5 {
      ret 1
    }
  }
  ret n
}
