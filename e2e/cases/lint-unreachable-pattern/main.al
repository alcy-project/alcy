fn f(x: i32) -> i32 {
  ret match x {
    _ => 1,
    0 => 2,
  }
}

fn main() -> i32 {
  ret f(0)
}
