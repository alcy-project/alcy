spec Display {
  fn show(self: &Self) -> str;
}
struct Point {
  x: i32,
}
impl Display for i32 {
  fn show(self: &Self) -> str {
    ret "a"
  }
}
fn main() -> i32 {
  ret 0
}
