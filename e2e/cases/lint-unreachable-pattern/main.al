fn f(x: i32) -> i32 {
  ret match x {
    _ => 1,
    0 => 2,
  }
}

fn g(x: i32) -> i32 {
  ret match x {
    1 => 1,
    1 => 2,
    _ => 3,
  }
}

fn main() -> i32 {
  ret f(0) + g(1)
}
