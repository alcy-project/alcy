fn id(x: &i32) -> &i32 {
  ret x
}

fn sum(x: &i32) -> i32 {
  mut r := x
  {
    p := x
    r = p
  }
  ret *r + *id(x)
}

fn main() -> i32 {
  n := 5
  ret sum(&n)
}
