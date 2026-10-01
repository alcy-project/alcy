spec Display {
  fn show(self: &Self) -> str;
}
struct Point {
  x: i32,
}
impl Display for Point {
  fn show(self: &Self) -> str {
    ret "a"
  }
  fn extra(self: &Self) -> str {
    ret "b"
  }
}
fn main() -> i32 {
  ret 0
}
